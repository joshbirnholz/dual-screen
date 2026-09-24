#!/usr/bin/env python3
"""Builds assets/stylus_hand.png: the stylus-hand symbol, lifted from the Wii U.

    pip install pillow
    python3 assets/make_stylus_hand.py path/to/screenshot [x y radius]

The symbol is the one the Wii U Virtual Console's Screen Settings draws on the
button that says which hand holds the stylus. As drawn there it is a left
hand -- it comes in from the left, the stylus's tip pointing down to the
right -- because it is what the left-handed layouts show. It is copied out of a 1280x800 capture of that
screen, where the round button is centred at (639.5, 508.5) with a radius of
53 -- pass other numbers for another capture.

The button is round there and square here, so the copy is made like this:

- The button is scaled up eight times and each pixel is sorted by brightness
  into the three things the symbol is made of: the blue-grey background, the
  white of the hand and the stylus's tip, and the dark navy of the stylus's
  far end and the back of the hand. Softly, not with a hard cut, so the edges
  stay smooth. The background is dropped, shading and all: it is the round
  button's, not the symbol's.
- Only the inside of the circle is read, a little short of its edge: the
  rim is a light ring with a thin dark shadow inside it, and neither is part
  of the symbol.
- The square is the circle's bounding box. Where the symbol ran out through
  the circle's edge -- the arm on the left, the back of the hand on the right
  -- it is carried straight on out to the square's edge, as it would have gone
  had the button been square to begin with.
- Then it is painted back as white on a transparent background -- the white
  parts solid, the navy ones a fainter white -- and scaled down to 128
  pixels square. Only its alpha is used: the menu tints it like the button
  glyphs, so it can go dark on a highlighted button.

The app uses it as it is for a left hand and mirrored for a right.
"""

import os
import sys

from PIL import Image

SIZE = 128
SCALE = 8
# How solid the parts that are navy on the Wii U are drawn, as white.
BACK_OPACITY = 0.4


def luminance(rgb):
    r, g, b = rgb[:3]
    return 0.299 * r + 0.587 * g + 0.114 * b


def main():
    if len(sys.argv) not in (2, 5):
        sys.exit(__doc__)
    cx, cy, radius = (639.5, 508.5, 53.0) if len(sys.argv) == 2 else map(float, sys.argv[2:5])
    shot = Image.open(sys.argv[1]).convert("RGB")

    box = (cx - radius, cy - radius, cx + radius, cy + radius)
    side = int(round(2 * radius * SCALE))
    big = shot.transform((side, side), Image.EXTENT, box, Image.BICUBIC)
    pixels = big.load()

    # The background is not flat -- it is shaded, darkening towards the rim
    # -- so it spans a band of brightness of its own. White is well above that
    # band and navy well below it; only what is clearly past its edges counts.
    def white_of(l):
        return min(1.0, max(0.0, (l - 150.0) / (235.0 - 150.0)))

    def navy_of(l):
        return min(1.0, max(0.0, (98.0 - l) / (98.0 - 84.0)))

    inner = (radius - 8.0) * SCALE
    centre = side / 2.0
    white = [[0.0] * side for _ in range(side)]
    navy = [[0.0] * side for _ in range(side)]
    for y in range(side):
        dy = y + 0.5 - centre
        if abs(dy) >= inner:
            continue
        reach = (inner * inner - dy * dy) ** 0.5
        left = int(centre - reach) + 1
        right = int(centre + reach) - 1
        for x in range(left, right + 1):
            l = luminance(pixels[x, y])
            white[y][x] = white_of(l)
            navy[y][x] = navy_of(l)
        # Out to the square's edges, from the last pixel read on each side.
        for x in range(0, left):
            white[y][x] = white[y][left]
            navy[y][x] = navy[y][left]
        for x in range(right + 1, side):
            white[y][x] = white[y][right]
            navy[y][x] = navy[y][right]

    # White on nothing: the white parts solid, the navy ones -- the far end of
    # the stylus and the hand behind it -- a fainter white, the way they sit
    # back on the Wii U. Only the alpha is used; the menu tints it.
    out = Image.new("RGBA", (side, side), (255, 255, 255, 0))
    painted = out.load()
    for y in range(side):
        for x in range(side):
            w, n = white[y][x], navy[y][x]
            alpha = w + (1.0 - w) * n * BACK_OPACITY
            painted[x, y] = (255, 255, 255, int(round(alpha * 255)))

    small = out.resize((SIZE, SIZE), Image.LANCZOS)
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "stylus_hand.png")
    small.save(path)
    print(f"wrote {path}")


if __name__ == "__main__":
    main()
