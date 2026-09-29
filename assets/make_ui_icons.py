#!/usr/bin/env python3
"""Regenerates assets/ui_icons.png: Data Management's icons, as one strip.

Like the button strip, only coverage is kept -- each cell is white where the
icon is -- so every icon is tinted for whatever it is drawn on.

The cells, in UiIcon order (see src/render/ui_icons.hpp):

    get, send, history, delete     the game page's four actions, drawn here:
                                   send is an arrow up out of a tray, and get
                                   the same turned upside down
    steam-deck, switch, pc         the devices, in a band across the middle
                                   of the cell, from open icon sets:
                                   assets/ui_icons/steamdeck.png and switch.png
                                   are Kenney's Input Prompts (CC0); pc.png is
                                   Phosphor's desktop-tower-fill (MIT), drawn
                                   from pc.svg beside it

There is no open icon of a Switch Lite, so a Lite is drawn as a Switch.

Run from the repository root:  python3 assets/make_ui_icons.py
"""

import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_button_icons import read_png, read_palette_png_alpha, write_png, near_segment  # noqa: E402

CELL = 96
SS = 4
UNIT = CELL / 36.0   # the actions are drawn on a 36-unit square
STROKE = 2.6

DEVICES = ["steamdeck", "switch", "pc"]


def rounded_box_distance(x, y, left, top, width, height, radius):
    cx, cy = left + width / 2.0, top + height / 2.0
    qx = abs(x - cx) - (width / 2.0 - radius)
    qy = abs(y - cy) - (height / 2.0 - radius)
    outside = math.hypot(max(qx, 0.0), max(qy, 0.0))
    return outside + min(max(qx, qy), 0.0) - radius


def in_action(name, x, y):
    """x, y in the 36-unit square. True where the icon is."""
    half = STROKE / 2.0
    if name == "get":
        return in_action("send", x, 36.0 - y)
    if name == "send":
        tray = abs(rounded_box_distance(x, y, 3, 20, 30, 12, 5)) <= half
        arrow = (near_segment(x, y, 18, 24, 18, 4) <= half or
                 near_segment(x, y, 12, 10, 18, 4) <= half or
                 near_segment(x, y, 24, 10, 18, 4) <= half)
        # The arrow stands in front of the tray: the tray's top edge is cut
        # away round it, so the two read as two things.
        gap = abs(x - 18) <= half + 2.2 and 17 <= y <= 23
        return arrow or (tray and not gap)
    if name == "history":
        return (abs(math.hypot(x - 18, y - 18) - 13) <= half or
                near_segment(x, y, 18, 10, 18, 18) <= half or
                near_segment(x, y, 18, 18, 23, 22) <= half)
    if name == "delete":
        return (near_segment(x, y, 8, 10, 28, 10) <= half or
                near_segment(x, y, 14, 10, 14, 6) <= half or
                near_segment(x, y, 14, 6, 22, 6) <= half or
                near_segment(x, y, 22, 6, 22, 10) <= half or
                near_segment(x, y, 11, 10, 12.5, 30) <= half or
                near_segment(x, y, 12.5, 30, 23.5, 30) <= half or
                near_segment(x, y, 23.5, 30, 25, 10) <= half)
    sys.exit("unknown icon " + name)


def render_action(name):
    cell = bytearray(CELL * CELL)
    for py in range(CELL):
        for px in range(CELL):
            hits = 0
            for sy in range(SS):
                for sx in range(SS):
                    if in_action(name, (px + (sx + 0.5) / SS) / UNIT, (py + (sy + 0.5) / SS) / UNIT):
                        hits += 1
            cell[py * CELL + px] = round(255 * hits / (SS * SS))
    return cell


def device_alpha(name):
    path = f"assets/ui_icons/{name}.png"
    data = open(path, "rb").read()
    colour = data[25]
    if colour == 3:
        return read_palette_png_alpha(path)
    w, h, rgba = read_png(path)
    return w, h, [[rgba[(y * w + x) * 4 + 3] for x in range(w)] for y in range(h)]


def render_device(name):
    """The icon's own extent, fitted to a band across the middle of the cell
    half as tall as it is wide, and centred in it. A handheld is wide and a
    tower is not: fitted to the band, the two come out looking the same size
    beside each other, where fitted to the square the tower would dwarf the
    handheld."""
    sw, sh, alpha = device_alpha(name)
    xs = [x for y in range(sh) for x in range(sw) if alpha[y][x] > 0]
    ys = [y for y in range(sh) for x in range(sw) if alpha[y][x] > 0]
    ex0, ex1, ey0, ey1 = min(xs), max(xs) + 1, min(ys), max(ys) + 1
    # Source pixels per cell pixel.
    step = max((ex1 - ex0) / (CELL * 0.96), (ey1 - ey0) / (CELL * 0.48))
    extent = step * CELL
    ecx, ecy = (ex0 + ex1) / 2.0, (ey0 + ey1) / 2.0

    def sample(sx, sy):
        sx, sy = sx - 0.5, sy - 0.5
        x0, y0 = math.floor(sx), math.floor(sy)
        fx, fy = sx - x0, sy - y0

        def at(x, y):
            if x < 0 or y < 0 or x >= sw or y >= sh:
                return 0
            return alpha[y][x]
        top = at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx
        bottom = at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx
        return top * (1 - fy) + bottom * fy

    cell = bytearray(CELL * CELL)
    for py in range(CELL):
        for px in range(CELL):
            total = 0.0
            for sy in range(SS):
                for sx in range(SS):
                    u = (px + (sx + 0.5) / SS) / CELL - 0.5
                    v = (py + (sy + 0.5) / SS) / CELL - 0.5
                    total += sample(ecx + u * extent, ecy + v * extent)
            cell[py * CELL + px] = round(total / (SS * SS))
    return cell


def main():
    cells = [render_action(name) for name in ["get", "send", "history", "delete"]]
    cells += [render_device(name) for name in DEVICES]
    width = CELL * len(cells)
    out = bytearray(width * CELL * 4)
    for index, cell in enumerate(cells):
        for y in range(CELL):
            for x in range(CELL):
                at = (y * width + index * CELL + x) * 4
                out[at:at + 4] = bytes((255, 255, 255, cell[y * CELL + x]))
    write_png("assets/ui_icons.png", width, CELL, bytes(out))
    print(f"assets/ui_icons.png: {len(cells)} cells, {width}x{CELL}")


if __name__ == "__main__":
    main()
