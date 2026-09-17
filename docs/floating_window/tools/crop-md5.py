"""md5 a rectangle of each screenshot, to prove two states are identical.

"Esc closed the bubble" and "a second press closed it" are only *shown* when the
after-capture matches the before-capture exactly. Eyeballing two PNGs proves
nothing about a bubble that closed but left a stale layer behind.

Crop first. A whole-screen md5 almost never matches, because the pointer is
parked on the toolbar button after the click and its hover / ink-drop state
differs from the untouched capture -- a real difference, but not the one under
test. Measured on one round of the floating-window harness: full-screen hashes
of `before` and `esc` differed, while the bubble rectangle was byte-identical.

Usage:
    python3 crop-md5.py X0 Y0 X1 Y1 shot1.png shot2.png ...

Pass 0 0 0 0 to hash the whole image.
"""
import hashlib
import sys

from PIL import Image

x0, y0, x1, y1 = (int(v) for v in sys.argv[1:5])
digests = {}
for path in sys.argv[5:]:
    im = Image.open(path).convert("RGB")
    if (x0, y0, x1, y1) != (0, 0, 0, 0):
        im = im.crop((x0, y0, x1, y1))
    # .tobytes() on the cropped image, not the PNG file: two PNGs of identical
    # pixels can still differ byte-for-byte (timestamps, filter choices).
    digest = hashlib.md5(im.tobytes()).hexdigest()
    digests.setdefault(digest, []).append(path)
    print(digest, path)

print()
for digest, paths in digests.items():
    if len(paths) > 1:
        print("identical:", ", ".join(paths))
