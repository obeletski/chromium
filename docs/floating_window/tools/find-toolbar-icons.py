"""Locate the toolbar icons in a screenshot, so a click target is measured.

The harness has to click a specific toolbar button, and a guessed x coordinate
produces a screenshot of a browser in which nothing happened -- which looks
exactly like the feature not working. So the coordinate is derived from the
capture instead.

Method, which is cruder than it sounds and works because the toolbar is a light
strip with a few dark glyphs on it:

  1. In the vertical band where the toolbar lives, pick the row containing the
     most dark pixels. That is the row through the middle of the icons.
  2. In that row, collect every column that has a dark pixel within +/-12px
     vertically, then group columns separated by <= 4px. Each group is one icon.
  3. The midpoint of a group is its centre.

Icons are reported left to right, so the floating-window button is identified by
its position in the sequence rather than by absolute pixels -- which keeps
working when the window size or the set of visible buttons changes.

Usage:
    python3 find-toolbar-icons.py shot.png [x0 x1] [y0 y1]

x0..x1 defaults to the right-hand quarter of the image (the extensions / profile
end of the toolbar, where the button was added); y0..y1 defaults to 55..100,
which is where the toolbar row falls on a 1400x900 window with no bookmarks bar.
"""
import sys

from PIL import Image

DARK = 120  # 8-bit luma below this counts as glyph rather than toolbar ground.

path = sys.argv[1]
im = Image.open(path).convert("L")
W, H = im.size

x0 = int(sys.argv[2]) if len(sys.argv) > 2 else W * 3 // 4
x1 = int(sys.argv[3]) if len(sys.argv) > 3 else W
y0 = int(sys.argv[4]) if len(sys.argv) > 4 else 55
y1 = int(sys.argv[5]) if len(sys.argv) > 5 else 100

best = None
for y in range(y0, min(y1, H)):
    dark = sum(1 for x in range(x0, x1) if im.getpixel((x, y)) < DARK)
    if best is None or dark > best[1]:
        best = (y, dark)
y, dark_count = best
print("toolbar row y = %d (%d dark px)" % (y, dark_count))
if dark_count == 0:
    sys.exit("no dark pixels in the band -- wrong y range, or the window had "
             "not painted yet when the screenshot was taken")

cols = [x for x in range(x0, x1)
        if any(im.getpixel((x, yy)) < DARK
               for yy in range(max(0, y - 12), min(H, y + 12)))]
if not cols:
    sys.exit("no icon columns found")

groups, cur = [], [cols[0]]
for x in cols[1:]:
    if x - cur[-1] <= 4:
        cur.append(x)
    else:
        groups.append(cur)
        cur = [x]
groups.append(cur)

print("icon centres (left to right):")
for g in groups:
    print("    click:%d,%d   (width %d)" % ((g[0] + g[-1]) // 2, y, len(g)))
