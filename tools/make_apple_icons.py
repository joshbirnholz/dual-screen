#!/usr/bin/env python3
"""Draws the iOS and Apple TV asset catalogs from the Dualscreen icon.

The icon is assets/app_icon.svg: three flat shapes, a navy field and two gray
screens stacked like a DS. It is simple enough to draw directly, so this does
that rather than rasterising the SVG, and every picture in both catalogs comes
from the one set of numbers below. Change the icon there and here together.

    python3 tools/make_apple_icons.py

Needs Pillow, and Inter Display SemiBold for the Apple TV's Top Shelf wordmark
(the font the Dualscreen wordmark is set in). The pictures it writes are
committed, so a Mac building the app needs neither.
"""

import json
import os
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
APPLE = os.path.join(ROOT, "apple")

# assets/app_icon.svg, on its 1024 x 1024 grid.
NAVY = (0x0E, 0x18, 0x2C)
TOP = (0xB4, 0xBA, 0xC6)
BOTTOM = (0x6B, 0x73, 0x85)
SCREEN_X, SCREEN_W, SCREEN_H, RADIUS = 282, 460, 316, 48
TOP_Y, BOTTOM_Y = 172, 536
GLYPH_TOP, GLYPH_BOTTOM = TOP_Y, BOTTOM_Y + SCREEN_H  # 172 .. 852

FONT_CANDIDATES = [
    "/usr/share/fonts/opentype/inter/InterDisplay-SemiBold.otf",
    "/Library/Fonts/InterDisplay-SemiBold.otf",
    os.path.expanduser("~/Library/Fonts/InterDisplay-SemiBold.otf"),
]

INFO = {"author": "xcode", "version": 1}


def supersample(size):
    """Drawn this many times larger and scaled down, for smooth edges. Less
    for the big Top Shelf pictures, which are smooth enough and would
    otherwise be hundreds of megabytes while being drawn."""
    return max(1, min(4, 8192 // max(size)))


def write_json(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump(data, f, indent=2)
        f.write("\n")


def draw_glyph(draw, cx, top, height):
    """The two screens, scaled so the pair is `height` tall, centred on cx."""
    s = height / (GLYPH_BOTTOM - GLYPH_TOP)
    w, h, r = SCREEN_W * s, SCREEN_H * s, RADIUS * s
    x0 = cx - w / 2
    for y, colour in ((TOP_Y, TOP), (BOTTOM_Y, BOTTOM)):
        y0 = top + (y - GLYPH_TOP) * s
        draw.rounded_rectangle((x0, y0, x0 + w, y0 + h), radius=r, fill=colour)
    return SCREEN_W * s  # width of the glyph


def canvas(size, opaque):
    w, h = size
    S = supersample(size)
    big = (w * S, h * S)
    if opaque:
        return Image.new("RGB", big, NAVY)
    return Image.new("RGBA", big, (0, 0, 0, 0))


def finish(img, size, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    img.resize(size, Image.LANCZOS).save(path, optimize=True)


# --- iOS ---------------------------------------------------------------------
#
# One 1024 x 1024 picture: Xcode 14 and later make every other size from it.
# Opaque, square and uncornered -- iOS rounds the corners itself, and rejects an
# icon with an alpha channel.

def ios_icon(path):
    img = canvas((1024, 1024), True)
    d = ImageDraw.Draw(img)
    S = supersample((1024, 1024))
    draw_glyph(d, 512 * S, GLYPH_TOP * S, (GLYPH_BOTTOM - GLYPH_TOP) * S)
    finish(img, (1024, 1024), path)


def build_ios():
    cat = os.path.join(APPLE, "Assets-iOS.xcassets")
    write_json(os.path.join(cat, "Contents.json"), {"info": INFO})
    icon = os.path.join(cat, "AppIcon.appiconset")
    ios_icon(os.path.join(icon, "AppIcon-1024.png"))
    write_json(os.path.join(icon, "Contents.json"), {
        "images": [{
            "filename": "AppIcon-1024.png",
            "idiom": "universal",
            "platform": "ios",
            "size": "1024x1024",
        }],
        "info": INFO,
    })


# --- Apple TV ----------------------------------------------------------------
#
# A tvOS icon is a stack of layers the system tilts against each other when it
# is focused: here the navy field at the back and the two screens in front, so
# the screens float over it. It is a landscape tile, 5:3, and the system crops
# its edges while it moves, so the screens keep well inside it. The Top Shelf
# pictures behind the app's row on the home screen are the logo, glyph and
# wordmark, on the same navy.

TV_GLYPH_FRACTION = 0.6  # of the tile's height


def tv_layer(size, scale, which, path):
    w, h = size[0] * scale, size[1] * scale
    if which == "back":
        img = canvas((w, h), True)
    else:
        img = canvas((w, h), False)
        d = ImageDraw.Draw(img)
        S = supersample((w, h))
        gh = h * TV_GLYPH_FRACTION * S
        draw_glyph(d, w * S / 2, (h * S - gh) / 2, gh)
    finish(img, (w, h), path)


def find_font():
    for path in FONT_CANDIDATES:
        if os.path.exists(path):
            return path
    sys.exit("Inter Display SemiBold not found; it is needed for the Top Shelf wordmark")


def top_shelf(size, path, font_path):
    w, h = size
    S = supersample((w, h))
    img = canvas((w, h), True)
    d = ImageDraw.Draw(img)
    # The logo's own proportions (icon/steamgriddb/logo): the glyph, a gap,
    # then "Dual" in the top screen's gray and "screen" in the bottom's.
    gh = h * 0.42 * S
    font = ImageFont.truetype(font_path, int(gh * 0.36))
    dual_w = d.textlength("Dual", font=font)
    screen_w = d.textlength("screen", font=font)
    glyph_w = SCREEN_W * gh / (GLYPH_BOTTOM - GLYPH_TOP)
    gap = gh * 0.17
    total = glyph_w + gap + dual_w + screen_w
    left = (w * S - total) / 2
    top = (h * S - gh) / 2
    draw_glyph(d, left + glyph_w / 2, top, gh)
    tx = left + glyph_w + gap
    ty = h * S / 2
    d.text((tx, ty), "Dual", font=font, fill=TOP, anchor="lm")
    d.text((tx + dual_w, ty), "screen", font=font, fill=BOTTOM, anchor="lm")
    finish(img, (w, h), path)


def build_tvos():
    cat = os.path.join(APPLE, "Assets-tvOS.xcassets")
    write_json(os.path.join(cat, "Contents.json"), {"info": INFO})
    brand = os.path.join(cat, "AppIcon.brandassets")
    font_path = find_font()

    stacks = [
        # (stack name, size, scales)
        ("App Icon.imagestack", (400, 240), (1, 2)),
        ("App Icon - App Store.imagestack", (1280, 768), (1,)),
    ]
    for name, size, scales in stacks:
        stack = os.path.join(brand, name)
        layers = []
        for layer in ("Front", "Back"):
            ldir = os.path.join(stack, layer + ".imagestacklayer")
            layers.append({"filename": layer + ".imagestacklayer"})
            write_json(os.path.join(ldir, "Contents.json"), {"info": INFO})
            images = []
            for scale in scales:
                fname = "%s-%dx%d@%dx.png" % (layer.lower(), size[0], size[1], scale)
                tv_layer(size, scale, layer.lower(),
                         os.path.join(ldir, "Content.imageset", fname))
                images.append({"filename": fname, "idiom": "tv", "scale": "%dx" % scale})
            write_json(os.path.join(ldir, "Content.imageset", "Contents.json"),
                       {"images": images, "info": INFO})
        write_json(os.path.join(stack, "Contents.json"), {"info": INFO, "layers": layers})

    shelves = [
        ("Top Shelf Image.imageset", (1920, 720)),
        ("Top Shelf Image Wide.imageset", (2320, 720)),
    ]
    for name, size in shelves:
        sdir = os.path.join(brand, name)
        images = []
        for scale in (1, 2):
            fname = "top-shelf-%dx%d@%dx.png" % (size[0], size[1], scale)
            top_shelf((size[0] * scale, size[1] * scale), os.path.join(sdir, fname), font_path)
            images.append({"filename": fname, "idiom": "tv", "scale": "%dx" % scale})
        write_json(os.path.join(sdir, "Contents.json"), {"images": images, "info": INFO})

    write_json(os.path.join(brand, "Contents.json"), {
        "assets": [
            {"filename": "App Icon - App Store.imagestack", "idiom": "tv",
             "role": "primary-app-icon", "size": "1280x768"},
            {"filename": "App Icon.imagestack", "idiom": "tv",
             "role": "primary-app-icon", "size": "400x240"},
            {"filename": "Top Shelf Image Wide.imageset", "idiom": "tv",
             "role": "top-shelf-image-wide", "size": "2320x720"},
            {"filename": "Top Shelf Image.imageset", "idiom": "tv",
             "role": "top-shelf-image", "size": "1920x720"},
        ],
        "info": INFO,
    })


if __name__ == "__main__":
    build_ios()
    build_tvos()
