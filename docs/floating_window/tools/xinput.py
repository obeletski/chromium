"""Synthesise real X pointer/key input through XTEST, via ctypes.

Why this exists at all: the thing under test is a `views::` toolbar button and
a `views::BubbleDialogDelegate`, both of which live in the **browser** process.
DevTools' `Input` domain (`Input.dispatchMouseEvent`) delivers events *inside a
renderer*, so it cannot reach them -- it reports success and nothing happens,
which is indistinguishable from the feature being broken. Events injected with
`XTestFakeButtonEvent` enter the X server's own event stream as though they came
from the hardware, so they travel the ordinary path into `ui::` and then Views.

ctypes rather than a package: nothing like `python-xlib` or `pyautogui` is
installed in this checkout, and `libX11`/`libXtst` are already present because
the build needs them.

Usage (DISPLAY must already point at the server, e.g. the harness's Xvfb):

    DISPLAY=:99 python3 xinput.py click:1325,83 sleep:0.5 key:Escape

Commands:  click:X,Y   move:X,Y   key:<XStringToKeysym name>   sleep:SECONDS
"""
import ctypes
import os
import sys
import time

x11 = ctypes.CDLL("libX11.so.6")
xtst = ctypes.CDLL("libXtst.so.6")

# Without this, ctypes truncates the returned Display* to a 32-bit int on x86-64
# and every later call gets a garbage pointer -- usually a segfault, sometimes
# silence.
x11.XOpenDisplay.restype = ctypes.c_void_p

_display_name = os.environ.get("DISPLAY", ":99").encode()
_d = x11.XOpenDisplay(_display_name)
if not _d:
    sys.exit("cannot open display %s" % _display_name.decode())
d = ctypes.c_void_p(_d)


def flush():
    x11.XFlush(d)


def move(x, y):
    # XWarpPointer with src=None moves the pointer in absolute root coordinates.
    x11.XWarpPointer(d, None, ctypes.c_void_p(x11.XDefaultRootWindow(d)),
                     0, 0, 0, 0, int(x), int(y))
    flush()
    # The sleeps below are load-bearing, not politeness. Warping and clicking
    # within one flush frequently delivers the press at the *previous* pointer
    # position, because the server has not yet processed the motion.
    time.sleep(0.3)


def click(x, y):
    move(x, y)
    xtst.XTestFakeButtonEvent(d, 1, True, 0)
    flush()
    time.sleep(0.08)
    xtst.XTestFakeButtonEvent(d, 1, False, 0)
    flush()
    # A bubble animates in; screenshotting sooner catches it half-drawn and
    # makes md5 comparisons between runs meaningless.
    time.sleep(1.5)


def key(keysym_name):
    # "Escape", "Return", "Tab", "a", ... -- any name XStringToKeysym knows.
    ks = x11.XStringToKeysym(keysym_name.encode())
    kc = x11.XKeysymToKeycode(d, ctypes.c_ulong(ks))
    xtst.XTestFakeKeyEvent(d, kc, True, 0)
    flush()
    time.sleep(0.08)
    xtst.XTestFakeKeyEvent(d, kc, False, 0)
    flush()
    time.sleep(1.5)


for cmd in sys.argv[1:]:
    kind, _, rest = cmd.partition(":")
    if kind == "click":
        cx, cy = rest.split(",")
        click(int(cx), int(cy))
        print("clicked", rest)
    elif kind == "move":
        cx, cy = rest.split(",")
        move(int(cx), int(cy))
        print("moved", rest)
    elif kind == "key":
        key(rest)
        print("key", rest)
    elif kind == "sleep":
        time.sleep(float(rest))
    else:
        sys.exit("unknown command: %s" % cmd)
