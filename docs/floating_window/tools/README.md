# UI harness: screenshotting browser-process UI

`chrome --headless --screenshot` photographs the **page**. A toolbar button, a
bubble, a menu — anything owned by the browser process rather than a renderer —
is not in that image, so none of it can be verified that way. Every screenshot
and every "clicking it opens the window" claim in
[`floating-window-implementation.md`](../floating-window-implementation.md),
[`floating-window-page-outlines.md`](../floating-window-page-outlines.md) and
[`floating-window-tab-summary.md`](../floating-window-tab-summary.md) came from
the scripts here instead: the real `chrome` on a virtual X display, driven with
synthetic **hardware** input, screenshotted with `scrot`.

## The pieces

| File | What it does |
|---|---|
| `ui-harness.sh` | Starts `Xvfb`, launches `out/Linux/chrome` on it, then walks a list of `--shot` / `--step` actions in order |
| `xinput.py` | Injects pointer and key events through XTEST (`libX11` + `libXtst` via `ctypes`) |
| `find-toolbar-icons.py` | Reads a screenshot and reports the centre of each toolbar icon, so the click coordinate is *measured* |
| `crop-md5.py` | Hashes a rectangle of several captures, to prove two states are identical |
| `cdp.py` | Minimal DevTools client: navigates a tab to a `chrome://` URL and dumps the rendered DOM |

Needs `Xvfb`, `scrot`, `python3-pil`, and `libXtst` (already present — the build
links it). No Python packages beyond Pillow; there is deliberately no
`websockets`, `pyautogui` or `selenium` dependency.

## A full round

```sh
mkdir -p /tmp/harness/pages
echo '<!doctype html><title>Harness A</title><h1>Alpha</h1><h2>Beta</h2>' \
    > /tmp/harness/pages/a.html

# 1. Launch and capture, so there is something to measure against.
WORK=/tmp/harness/run docs/floating_window/tools/ui-harness.sh \
    --page file:///tmp/harness/pages/a.html --shot before

# 2. Find the button. Icons are listed left to right; on a default profile the
#    floating-window button is the one between the bookmark star and the
#    profile chip.
python3 docs/floating_window/tools/find-toolbar-icons.py /tmp/harness/run/before.png
#   toolbar row y = 76 (111 dark px)
#   icon centres (left to right):
#       click:1264,76   (width 14)     <- bookmark star
#       click:1313,76   (width 16)     <- floating window
#       click:1350,76   (width 16)     <- profile
#       click:1386,76   (width 2)      <- three-dot menu

# 3. Drive it.
WORK=/tmp/harness/run2 docs/floating_window/tools/ui-harness.sh \
    --page file:///tmp/harness/pages/a.html \
    --window file:///tmp/harness/pages/a.html \
    --shot before --step click:1313,76 --shot open \
    --step key:Escape --shot esc

# 4. Prove the Esc actually restored the prior state.
python3 docs/floating_window/tools/crop-md5.py 640 100 1420 420 \
    /tmp/harness/run2/{before,open,esc}.png
```

`WORK` holds the throwaway profile, the PNGs, `chrome.log` and `xvfb.log`;
nothing is written into the tree. `OUT_DIR` (default `out/Linux`), `DISPLAY_NUM`
(`:99`), `SCREEN`, `WINDOW`, `SETTLE` and `PORT` are the other knobs — override
`DISPLAY_NUM` and `PORT` if another session on this checkout is already running
a harness.

## Traps this encodes

**CDP cannot click browser UI.** `Input.dispatchMouseEvent` is delivered inside
a renderer. Aimed at a `views::` toolbar button it reports success and does
nothing, which reads exactly like a broken feature. XTEST events enter the X
server's event stream as though from hardware, so they take the ordinary path
into `ui::` and Views. This is the single reason the harness is built this way.

**Never guess the click coordinate.** A wrong `x` yields a screenshot of a
browser in which nothing happened — again indistinguishable from a broken
feature. `find-toolbar-icons.py` derives it from the capture: darkest row in the
toolbar band, then dark columns clustered into icons.

**Crop before comparing.** A whole-screen md5 of `before` and `esc` will differ
even on a clean close, because the pointer is parked on the button and its hover
state is now drawn. On the run above the full-screen hashes differed while the
bubble rectangle was byte-identical.

**`SETTLE` is not padding.** A debug / component build paints the toolbar many
seconds after the process starts. Shoot early and `find-toolbar-icons.py`
reports "no dark pixels in the band", which looks like a bad `y` range.

**The sleeps in `xinput.py` are load-bearing.** Warping and clicking within one
flush often delivers the press at the previous pointer position, and a bubble
needs roughly 1.5 s to finish animating before the next capture is meaningful.

**`chrome://` pages resist the easy routes.** `/json/new` refuses a `chrome://`
URL and `--headless` accepts only one target, so neither opens
`chrome://floating-window` for markup inspection. `cdp.py` attaches to an
existing `about:blank` tab and drives `Page.navigate`, then reads
`document.documentElement.outerHTML` *after* `Page.loadEventFired` — the DOM the
page's script built, not the HTML the WebUI data source served.

**Tear down by profile path.** More than one session can share this checkout, so
`ui-harness.sh` matches `pkill -f "user-data-dir=$PROFILE"` and never
`pkill chrome`.

Verified end to end on 2026-09-17 against `out/Linux/chrome` on the
`floating-window` branch: bubble opens on the click, Esc closes it, and the
`before`/`esc` bubble rectangles hash identically.
