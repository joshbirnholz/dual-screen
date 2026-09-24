#!/usr/bin/env python3
"""Draws assets/stylus_hand.png: a right hand holding a stylus.

    pip install pillow
    python3 assets/make_stylus_hand.py

The quick menu's Screen Layout section draws it on the button that says which
hand holds the stylus, mirrored for a left hand -- the same idea as the Wii U
Virtual Console's own Screen Settings, drawn afresh here. Like the button
glyphs it is black artwork whose alpha is all that is used, so the menu can
tint it for whatever it sits on.

Drawn at eight times the size and scaled down, which is the whole of the
anti-aliasing. Each part of the hand is cut out of what is under it by a thin
margin before it is filled, so the fingers read as fingers against the
stylus rather than as one blob.
"""

import math
import os

from PIL import Image, ImageDraw

SIZE = 128
SCALE = 8
CANVAS = SIZE * SCALE
MARGIN = 0.028  # the gap around each part, as a fraction of the icon


def point(x, y):
    return (x * CANVAS, y * CANVAS)


def capsule(start, end, radius, steps=24):
    """A stroke with round ends, from `start` to `end`, as a polygon."""
    (x0, y0), (x1, y1) = start, end
    angle = math.atan2(y1 - y0, x1 - x0)
    outline = []
    for index in range(steps + 1):
        a = angle - math.pi / 2 + math.pi * index / steps
        outline.append(point(x1 + math.cos(a) * radius, y1 + math.sin(a) * radius))
    for index in range(steps + 1):
        a = angle + math.pi / 2 + math.pi * index / steps
        outline.append(point(x0 + math.cos(a) * radius, y0 + math.sin(a) * radius))
    return outline


def ellipse(cx, cy, rx, ry, angle=0.0, steps=64):
    outline = []
    for index in range(steps):
        t = 2 * math.pi * index / steps
        x, y = math.cos(t) * rx, math.sin(t) * ry
        outline.append(point(cx + x * math.cos(angle) - y * math.sin(angle),
                             cy + x * math.sin(angle) + y * math.cos(angle)))
    return outline


def grow(shape_fn, amount):
    """The same shape, fattened by `amount`: used to cut the gap around it."""
    return shape_fn(amount)


def main():
    image = Image.new("L", (CANVAS, CANVAS), 0)
    draw = ImageDraw.Draw(image)

    def put(shape_fn):
        draw.polygon(shape_fn(MARGIN), fill=0)
        draw.polygon(shape_fn(0.0), fill=255)

    # Everything is placed along the stylus: `along` from 0 at its tip, at
    # the bottom left, to 1 at the top right; `across` at right angles to it,
    # positive on the side the fingers curl round.
    def at(along, across):
        return (0.19 + along * 0.66 + across * 0.7071,
                0.81 - along * 0.66 + across * 0.7071)

    width = 0.042

    def stylus(extra):
        w = (width + extra) * 0.7071
        tip, neck, top = at(0.0, 0.0), at(0.14, 0.0), at(0.98, 0.0)
        return [point(tip[0] - extra, tip[1] + extra),
                point(neck[0] + w, neck[1] + w),
                point(top[0] + w, top[1] + w),
                point(top[0] - w, top[1] - w),
                point(neck[0] - w, neck[1] - w)]

    put(stylus)

    # The hand, coming in from the top right: one solid fist -- palm and
    # wrist -- closed round the stylus, the thumb over it on the near side,
    # and the creases between the curled fingers cut into it. Solid shapes
    # and a few cuts, rather than every finger drawn, is what still reads as
    # a hand at the size a button draws it.
    def fist(extra):
        return ellipse(*at(0.56, 0.12), 0.25 + extra, 0.205 + extra, angle=-math.pi / 4)

    def arm(extra):
        return capsule(at(0.62, 0.20), at(1.25, 0.36), 0.18 + extra)

    # Fist and wrist are one shape, so no line is cut between them.
    draw.polygon(fist(MARGIN), fill=0)
    draw.polygon(arm(MARGIN), fill=0)
    draw.polygon(fist(0.0), fill=255)
    draw.polygon(arm(0.0), fill=255)
    put(lambda e: capsule(at(0.36, -0.07), at(0.66, -0.12), 0.07 + e))

    def cut(start, end):
        draw.polygon(capsule(start, end, MARGIN * 0.55), fill=0)

    # The index finger along the stylus, and the two fingers under it.
    cut(at(0.33, 0.10), at(0.62, 0.12))
    cut(at(0.40, 0.21), at(0.64, 0.24))
    cut(at(0.50, 0.31), at(0.68, 0.33))

    # Cut to the round button it sits on, so the wrist runs out through the
    # button's edge rather than stopping at a straight one.
    round_mask = Image.new("L", (CANVAS, CANVAS), 0)
    ImageDraw.Draw(round_mask).ellipse(
        [CANVAS * 0.02, CANVAS * 0.02, CANVAS * 0.98, CANVAS * 0.98], fill=255)
    image = Image.composite(image, Image.new("L", (CANVAS, CANVAS), 0), round_mask)

    small = image.resize((SIZE, SIZE), Image.LANCZOS)
    art = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    art.putalpha(small)
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "stylus_hand.png")
    art.save(out)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
