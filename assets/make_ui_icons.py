#!/usr/bin/env python3
"""Regenerates assets/ui_icons.png: Data Management's icons, as one strip.

Like the button strip, only coverage is kept -- each cell is white where the
icon is -- so every icon is tinted for whatever it is drawn on.

The cells, in UiIcon order (see src/ui/data_management_screen.cpp):

    get, send, history, delete     the game page's actions, drawn here:
                                   send is an arrow up out of a tray, and get
                                   the same turned upside down
    steam-deck, switch, pc         the devices, in a band across the middle
                                   of the cell, from open icon sets:
                                   assets/ui_icons/steamdeck.png is Kenney's
                                   Input Prompts (CC0); switch.png is the
                                   console's outline as the user gave it; pc.png is
                                   Phosphor's desktop-tower-fill (MIT), drawn
                                   from pc.svg beside it
    sync                           two arrows round a circle
    android                        an Android device: the robot's head, drawn
                                   here -- a half disc, two antennae, and two
                                   eyes cut out of it
    wiiu-gamepad                   a Wii U GamePad, for Controller Order:
                                   Kenney's solid one (CC0),
                                   assets/ui_icons/wiiu_gamepad.png
    tv                             a television, drawn here: a screen's
                                   heavy rounded outline on a stem and base,
                                   for the Wii U menu's which-screen icon
    wiiu-pro, classic-pro,         what a Wii U game has a player as, drawn
    wii-remote, wii-remote-nunchuk here as solid shapes with their sticks
    wii-remote-classic             and buttons cut out, after the outlines
                                   on the back of a Wii U game's box; a
                                   Wii Remote + Classic Controller is drawn
                                   as the Classic Controller Pro alone

A Switch and a Switch Lite are drawn alike. New cells go on the end, so the
ones before them keep their places.

Run from the repository root:  python3 assets/make_ui_icons.py
"""

import math
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_button_icons import write_png  # noqa: E402


def near_segment(x, y, x0, y0, x1, y1):
    """How far (x, y) is from the segment (x0, y0)-(x1, y1)."""
    dx, dy = x1 - x0, y1 - y0
    length = dx * dx + dy * dy
    t = 0.0 if length == 0 else max(0.0, min(1.0, ((x - x0) * dx + (y - y0) * dy) / length))
    return math.hypot(x - (x0 + t * dx), y - (y0 + t * dy))


def read_png_alpha(path):
    """A PNG's alpha as rows -- 8-bit RGBA, or palette with transparency."""
    data = open(path, "rb").read()
    pos, idat, trns = 8, b"", b""
    w = h = depth = colour = 0
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            w, h, depth, colour = struct.unpack(">IIBB", body[:10])
            if body[12] != 0:
                sys.exit(path + ": interlaced")
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
    rows = []
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
        row = []
        for x in range(w):
            if colour == 6:
                row.append(line[x * 4 + 3])
            else:
                bit = x * depth
                index = (line[bit // 8] >> (8 - depth - bit % 8)) & ((1 << depth) - 1)
                row.append(trns[index] if index < len(trns) else 255)
        rows.append(row)
    return w, h, rows

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
    if name == "sync":
        # Two arrows chasing each other round a circle.
        r = 11.0
        d = math.hypot(x - 18, y - 18)
        angle = math.degrees(math.atan2(y - 18, x - 18)) % 360
        on_arc = abs(d - r) <= half and (20 <= angle <= 160 or 200 <= angle <= 340)
        heads = False
        for end in (160.0, 340.0):
            # The arrowhead at the end of each arc, pointing on round it.
            a = math.radians(end)
            px, py = 18 + r * math.cos(a), 18 + r * math.sin(a)
            tx, ty = -math.sin(a), math.cos(a)          # the way round
            nx, ny = math.cos(a), math.sin(a)           # outward
            for side in (1, -1):
                bx = px - tx * 4.5 + nx * 4.5 * side
                by = py - ty * 4.5 + ny * 4.5 * side
                if near_segment(x, y, px, py, bx, by) <= half:
                    heads = True
        return on_arc or heads
    if name == "tv":
        frame = -2.8 <= rounded_box_distance(x, y, 4, 7, 28, 19, 3.5) <= 0
        stem = 16.6 <= x <= 19.4 and 25.5 <= y <= 29
        base = rounded_box_distance(x, y, 11, 28.5, 14, 5, 2.2) <= 0 and y <= 31
        return frame or stem or base
    if name == "delete":
        return (near_segment(x, y, 8, 10, 28, 10) <= half or
                near_segment(x, y, 14, 10, 14, 6) <= half or
                near_segment(x, y, 14, 6, 22, 6) <= half or
                near_segment(x, y, 22, 6, 22, 10) <= half or
                near_segment(x, y, 11, 10, 12.5, 30) <= half or
                near_segment(x, y, 12.5, 30, 23.5, 30) <= half or
                near_segment(x, y, 23.5, 30, 25, 10) <= half)
    if name in WII_U_CONTROLLERS:
        return in_controller(name, x, y)
    sys.exit("unknown icon " + name)


# The controllers a Wii U game can be handed, as solid shapes with their
# sticks and buttons cut out -- after the outlines on the back of a Wii U
# game's box that say which it plays with.
WII_U_CONTROLLERS = ["wiiu-pro", "classic-pro", "wii-remote", "wii-remote-nunchuk", "wii-remote-classic"]


def in_box(x, y, left, top, width, height, radius):
    return rounded_box_distance(x, y, left, top, width, height, radius) <= 0


def in_circle(x, y, cx, cy, r):
    return math.hypot(x - cx, y - cy) <= r


def in_cross(x, y, cx, cy, arm, half):
    return ((abs(x - cx) <= arm and abs(y - cy) <= half) or
            (abs(y - cy) <= arm and abs(x - cx) <= half))


def in_face_buttons(x, y, cx, cy, spread, r):
    return any(in_circle(x, y, cx + dx * spread, cy + dy * spread, r)
               for dx, dy in ((0, -1), (1, 0), (0, 1), (-1, 0)))


def in_pad(x, y, sticks_low):
    """A two-handed pad on the 36-unit square: a wide body, a grip under
    each end. The Pro Controller has its left stick high and its right low;
    the Classic Controller Pro has its D-pad and buttons high, both sticks
    low between them."""
    body = (in_box(x, y, 3, 9, 30, 14, 6) or
            near_segment(x, y, 8.5, 18, 6.5, 28) <= 5 or
            near_segment(x, y, 27.5, 18, 29.5, 28) <= 5)
    if sticks_low:
        holes = (in_cross(x, y, 9.5, 14.5, 2.9, 1.0) or in_face_buttons(x, y, 26.5, 14.5, 2.5, 1.05) or
                 in_circle(x, y, 13.8, 20.3, 2.6) or in_circle(x, y, 22.2, 20.3, 2.6))
    else:
        holes = (in_circle(x, y, 9.5, 14.5, 2.9) or in_cross(x, y, 13.6, 20.6, 2.6, 0.95) or
                 in_face_buttons(x, y, 26.5, 14.5, 2.5, 1.05) or in_circle(x, y, 22.4, 20.6, 2.6))
    return body and not holes


def in_remote(x, y, cx, width):
    """A Wii Remote standing up, `width` wide round `cx`: its D-pad, A, and
    1 and 2 near the foot cut out."""
    half = width / 2.0
    body = in_box(x, y, cx - half, 1.5, width, 33, half)
    unit = width / 9.0
    holes = (in_cross(x, y, cx, 7.5, 2.7 * unit, 0.95 * unit) or in_circle(x, y, cx, 13.8, 1.9 * unit) or
             in_circle(x, y, cx, 25, 1.05 * unit) or in_circle(x, y, cx, 28.6, 1.05 * unit))
    return body and not holes


def in_taper(x, y, x0, y0, r0, x1, y1, r1):
    """A capsule from (x0, y0) to (x1, y1) whose radius goes from r0 to r1."""
    dx, dy = x1 - x0, y1 - y0
    length = math.hypot(dx, dy)
    steps = 48
    for step in range(steps + 1):
        t = step / steps
        if math.hypot(x - (x0 + t * dx), y - (y0 + t * dy)) <= r0 + (r1 - r0) * t:
            return True
    return False


def in_controller(name, x, y):
    if name == "wiiu-pro":
        return in_pad(x, y, False)
    if name == "classic-pro":
        return in_pad(x, y, True)
    if name == "wii-remote":
        return in_remote(x, y, 18, 9)
    if name == "wii-remote-nunchuk":
        # The Nunchuk on the left, the remote beside it: a round head with
        # its stick cut out, tapering to a narrower foot.
        nunchuk = in_taper(x, y, 10, 11.5, 6.6, 10, 29.5, 4.2)
        stick = in_circle(x, y, 10, 11.5, 2.6)
        return in_remote(x, y, 26.5, 8) or (nunchuk and not stick)
    if name == "wii-remote-classic":
        # The Classic Controller Pro alone: the remote it is plugged into is
        # there, but not what the player holds.
        return in_pad(x, y, True)
    sys.exit("unknown controller " + name)


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


def android_alpha():
    """The robot's head on a 256-unit square, as coverage: a half disc with
    two antennae, the eyes cut out."""
    size, ss = 256, 3
    out = []
    for y in range(size):
        row = []
        for x in range(size):
            hits = 0
            for sy in range(ss):
                for sx in range(ss):
                    px, py = x + (sx + 0.5) / ss, y + (sy + 0.5) / ss
                    head = py <= 172 and math.hypot(px - 128, py - 172) <= 108
                    antenna = (near_segment(px, py, 84, 92, 60, 52) <= 7 or
                               near_segment(px, py, 172, 92, 196, 52) <= 7)
                    eye = (math.hypot(px - 90, py - 128) <= 12 or
                           math.hypot(px - 166, py - 128) <= 12)
                    if (head or antenna) and not eye:
                        hits += 1
            row.append(round(255 * hits / (ss * ss)))
        out.append(row)
    return size, size, out


def device_alpha(name):
    if name == "android":
        return android_alpha()
    return read_png_alpha(f"assets/ui_icons/{name}.png")


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
    cells += [render_action("sync")]
    cells += [render_device("android"), render_device("wiiu_gamepad")]
    cells += [render_action("tv")]
    cells += [render_action(name) for name in WII_U_CONTROLLERS]
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
