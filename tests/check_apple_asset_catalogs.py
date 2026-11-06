#!/usr/bin/env python3
"""The iOS and Apple TV asset catalogs, checked from a machine that is not a Mac.

An asset catalog is compiled by actool, which exists only on a Mac, and a
malformed one fails the Apple build outright: a Contents.json naming a file
that is not there, a picture the wrong size for its slot, an iOS icon with an
alpha channel. None of that is visible from here unless something reads it.

Usage: check_apple_asset_catalogs.py <apple dir>
"""

import json
import os
import struct
import sys

errors = []


def fail(message):
    errors.append(message)


def png_info(path):
    """(width, height, has_alpha) from a PNG's header."""
    with open(path, "rb") as f:
        head = f.read(33)
    if head[:8] != b"\x89PNG\r\n\x1a\n" or head[12:16] != b"IHDR":
        fail("%s: not a PNG" % path)
        return None
    width, height, _depth, colour = struct.unpack(">IIBB", head[16:26])
    # Colour types 4 and 6 carry alpha. (A tRNS chunk can too; nothing here
    # writes one.)
    return width, height, colour in (4, 6)


def load(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError) as e:
        fail("%s: %s" % (path, e))
        return {}


def check_imageset(directory, size, opaque):
    """Every picture named exists, is the slot's size at its scale, and every
    picture there is named."""
    contents = load(os.path.join(directory, "Contents.json"))
    named = set()
    images = contents.get("images", [])
    if not images:
        fail("%s: no images" % directory)
    for image in images:
        name = image.get("filename")
        if not name:
            fail("%s: a slot with no picture in it" % directory)
            continue
        named.add(name)
        path = os.path.join(directory, name)
        if not os.path.isfile(path):
            fail("%s: names %s, which is not there" % (directory, name))
            continue
        info = png_info(path)
        if not info:
            continue
        scale = int(image.get("scale", "1x").rstrip("x"))
        slot = image.get("size", "%dx%d" % size)
        w, h = (int(float(v) * scale) for v in slot.split("x"))
        if (info[0], info[1]) != (w, h):
            fail("%s is %dx%d, its slot wants %dx%d" % (path, info[0], info[1], w, h))
        if opaque and info[2]:
            fail("%s has an alpha channel; this slot must be opaque" % path)
    for name in os.listdir(directory):
        if name.endswith(".png") and name not in named:
            fail("%s: %s is there but not named in Contents.json" % (directory, name))


def check_ios(apple):
    catalog = os.path.join(apple, "Assets-iOS.xcassets")
    load(os.path.join(catalog, "Contents.json"))
    icon = os.path.join(catalog, "AppIcon.appiconset")
    if not os.path.isdir(icon):
        fail("%s: no AppIcon.appiconset, which CMakeLists.txt names" % catalog)
        return
    # iOS rejects an app icon with alpha in it.
    check_imageset(icon, (1024, 1024), opaque=True)


def check_tvos(apple):
    catalog = os.path.join(apple, "Assets-tvOS.xcassets")
    load(os.path.join(catalog, "Contents.json"))
    brand = os.path.join(catalog, "AppIcon.brandassets")
    if not os.path.isdir(brand):
        fail("%s: no AppIcon.brandassets, which CMakeLists.txt names" % catalog)
        return
    roles = set()
    for asset in load(os.path.join(brand, "Contents.json")).get("assets", []):
        name, role = asset.get("filename", ""), asset.get("role", "")
        roles.add(role)
        size = tuple(int(v) for v in asset.get("size", "0x0").split("x"))
        path = os.path.join(brand, name)
        if not os.path.isdir(path):
            fail("%s: names %s, which is not there" % (brand, name))
            continue
        if name.endswith(".imagestack"):
            layers = load(os.path.join(path, "Contents.json")).get("layers", [])
            # A tvOS icon is two to five layers; one is not a stack.
            if not 2 <= len(layers) <= 5:
                fail("%s: %d layers, tvOS wants 2 to 5" % (path, len(layers)))
            for i, layer in enumerate(layers):
                ldir = os.path.join(path, layer.get("filename", ""))
                if not os.path.isdir(ldir):
                    fail("%s: names layer %s, which is not there" % (path, layer))
                    continue
                load(os.path.join(ldir, "Contents.json"))
                # The bottom layer is what shows through everything; it has
                # to cover the tile.
                check_imageset(os.path.join(ldir, "Content.imageset"), size,
                               opaque=(i == len(layers) - 1))
        else:
            check_imageset(path, size, opaque=True)
    for role in ("primary-app-icon", "top-shelf-image", "top-shelf-image-wide"):
        if role not in roles:
            fail("%s: nothing in the %s role" % (brand, role))


def main():
    apple = sys.argv[1]
    check_ios(apple)
    check_tvos(apple)
    cmake = open(os.path.join(apple, "CMakeLists.txt")).read()
    for needle in ("Assets-iOS.xcassets", "Assets-tvOS.xcassets",
                   "ASSETCATALOG_COMPILER_APPICON_NAME \"AppIcon\""):
        if needle not in cmake:
            fail("apple/CMakeLists.txt does not mention %s" % needle)
    for e in errors:
        print("error: " + e)
    if errors:
        sys.exit(1)
    print("asset catalogs: ok")


if __name__ == "__main__":
    main()
