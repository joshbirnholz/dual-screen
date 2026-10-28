#!/usr/bin/env python3
"""Regenerates assets/button_icons.png: the sheet of button artwork.

Every cell is one of Kenney's Input Prompts (CC0, assets/kenney/, licence
beside them): a white shape with its letter or symbol knocked out of it. Only
the alpha is kept, so one sheet tints correctly against a dark backdrop and a
white dialog alike -- the colour is the shape and whatever is behind shows
through the symbol.

Every pad's whole set is here -- face buttons, the d-pad, both sticks and
their clicks, shoulders, triggers, start and select, home and capture -- so a
prompt can name any button in whichever pad's printing the player chose.

The cells are 64 pixels square, sixteen to a row, in the order of GlyphCell
in src/render/button_glyphs.hpp, which this list must match.

Run from the repository root:  python3 assets/make_button_icons.py
"""

import struct
import sys
import zlib

CELL = 64
COLUMNS = 16
SOURCE = "assets/kenney/{}.png"
OUTPUT = "assets/button_icons.png"

# GlyphCell order.
CELLS = [
    # A Switch's, which the Nintendo style draws.
    "switch_button_a", "switch_button_b", "switch_button_x", "switch_button_y",
    "switch_stick_l_press", "switch_stick_r_press",
    "switch_button_plus", "switch_button_minus",
    "switch_button_l", "switch_button_r", "switch_button_zl", "switch_button_zr",
    "switch_dpad_up_outline", "switch_dpad_down_outline",
    "switch_dpad_left_outline", "switch_dpad_right_outline", "switch_dpad",
    "switch_stick_l", "switch_stick_r", "switch_button_home", "switch_button_sync",
    # A PlayStation's.
    "playstation_button_circle", "playstation_button_cross",
    "playstation_button_triangle", "playstation_button_square",
    "playstation_button_l3", "playstation_button_r3",
    "playstation5_button_options", "playstation5_button_create",
    "playstation_trigger_l1", "playstation_trigger_r1",
    "playstation_trigger_l2", "playstation_trigger_r2",
    "playstation_dpad_up_outline", "playstation_dpad_down_outline",
    "playstation_dpad_left_outline", "playstation_dpad_right_outline", "playstation_dpad",
    "playstation_stick_l", "playstation_stick_r", "playstation5_button_mute",
    # An Xbox pad's.
    "xbox_button_a", "xbox_button_b", "xbox_button_x", "xbox_button_y",
    "xbox_ls", "xbox_rs", "xbox_button_menu", "xbox_button_view",
    "xbox_lb", "xbox_rb", "xbox_lt", "xbox_rt",
    "xbox_dpad_up_outline", "xbox_dpad_down_outline",
    "xbox_dpad_left_outline", "xbox_dpad_right_outline", "xbox_dpad",
    "xbox_stick_l", "xbox_stick_r", "xbox_guide", "xbox_button_share",
    # Generic: four dots with the one to press filled in, east, south,
    # north, west -- and a Steam Controller's plain shoulders and triggers,
    # and its start and back for start and select.
    "switch_buttons_right_outline", "switch_buttons_down_outline",
    "switch_buttons_up_outline", "switch_buttons_left_outline",
    "controller_button_l1", "controller_button_r1",
    "controller_button_l2", "controller_button_r2",
    "steam_button_start_icon", "steam_button_back_icon",
]


def read_alpha(path):
    """The alpha of a PNG -- 8-bit RGBA, or palette with transparency."""
    data = open(path, "rb").read()
    pos, idat, plte, trns = 8, b"", b"", b""
    w = h = depth = colour = 0
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            w, h, depth, colour = struct.unpack(">IIBB", body[:10])
            if body[12] != 0:
                sys.exit(path + ": interlaced")
        elif kind == b"PLTE":
            plte = body
        elif kind == b"tRNS":
            trns = body
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    channels = {3: 1, 6: 4}.get(colour)
    if channels is None or (colour == 6 and depth != 8):
        sys.exit(path + ": expected palette or 8-bit RGBA")
    bits = depth * channels
    stride = (w * bits + 7) // 8
    step = max(1, bits // 8)
    raw, prev, p = zlib.decompress(idat), bytearray(stride), 0
    alpha = []
    for _ in range(h):
        kind, p = raw[p], p + 1
        line = bytearray(raw[p:p + stride])
        p += stride
        for i in range(stride):
            a = line[i - step] if i >= step else 0
            b = prev[i]
            c = prev[i - step] if i >= step else 0
            if kind == 1:
                line[i] = (line[i] + a) & 255
            elif kind == 2:
                line[i] = (line[i] + b) & 255
            elif kind == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif kind == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        for x in range(w):
            if colour == 6:
                alpha.append(line[x * 4 + 3])
            else:
                bit = x * depth
                index = (line[bit // 8] >> (8 - depth - bit % 8)) & ((1 << depth) - 1)
                alpha.append(trns[index] if index < len(trns) else 255)
    if (w, h) != (CELL, CELL):
        sys.exit("{}: {}x{}, expected {}x{}".format(path, w, h, CELL, CELL))
    return alpha


def write_png(path, w, h, rgba):
    def chunk(kind, body):
        return (struct.pack(">I", len(body)) + kind + body +
                struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF))
    raw = b"".join(b"\x00" + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    with open(path, "wb") as out:
        out.write(b"\x89PNG\r\n\x1a\n")
        out.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)))
        out.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        out.write(chunk(b"IEND", b""))


def main():
    rows = (len(CELLS) + COLUMNS - 1) // COLUMNS
    width, height = CELL * COLUMNS, CELL * rows
    rgba = bytearray(width * height * 4)
    for index, name in enumerate(CELLS):
        alpha = read_alpha(SOURCE.format(name))
        left, top = (index % COLUMNS) * CELL, (index // COLUMNS) * CELL
        for y in range(CELL):
            for x in range(CELL):
                at = ((top + y) * width + left + x) * 4
                rgba[at + 3] = alpha[y * CELL + x]
    write_png(OUTPUT, width, height, rgba)
    print("{}: {} cells, {}x{}".format(OUTPUT, len(CELLS), width, height))


if __name__ == "__main__":
    main()
