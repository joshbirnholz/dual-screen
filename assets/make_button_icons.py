#!/usr/bin/env python3
"""Regenerates assets/button_icons.png: the strip of button artwork.

Every cell is a filled disc with its symbol knocked *out* of it, so one sheet
tints correctly against a dark backdrop and a white dialog alike -- the colour
is the disc and whatever is behind shows through the symbol.

The first eight cells are the original Nintendo-style artwork and are copied
from the existing file rather than redrawn: they were drawn by hand and this
cannot reproduce their letterforms. Everything after them is generated here,
to the geometry measured off those eight:

    cell 49 x 65, the disc bottom-aligned with 16px of headroom above it for
    the arrow that stands over a stick click
    disc centre (23.5, 40.0), radius 24
    symbols span 27px and are stroked 5px

After those come four positional cells, one per face button: four dots
with the one to press filled in, for a pad whose letters cannot be trusted
to mean the same corner as the prompt's. They are drawn from
assets/positional/switch_buttons_*_outline.png -- white on transparent, so
only their alpha is taken, the same as every other cell -- scaled to the
disc every other button fills.

Last come the shoulder buttons, L and R as each pad prints them: a Switch's
L and R, a Steam Controller's L1 and R1 (for the PlayStation and positional
styles -- the user's choice of the plainest), an Xbox pad's LB and RB. From
Kenney's Input Prompts (CC0), assets/shoulders/, filled shapes with the
letters already knocked out, so again only their alpha is taken. Each is fitted
to the square the disc sits in, bottom-aligned like it, so a shoulder glyph
lines up with a face button beside it.

Run from the repository root:  python3 assets/make_button_icons.py
"""

import math
import struct
import sys
import zlib

CELL_W, CELL_H = 49, 65
CX, CY, R = 23.5, 40.0, 24.0
STROKE = 5.0
SPAN = 13.5          # half the width a symbol may occupy
SS = 4               # supersampling, for the antialiased rim

# Cells 8 onward, in order. See glyph_cell() in button_glyphs.cpp.
GENERATED = ["cross", "circle", "triangle", "square", "bars", "view", "create"]

# And after those, the positional cells, in GlyphCell order: east, south,
# north, west -- the order of ButtonGlyph A, B, X, Y.
POSITIONAL = ["right", "down", "up", "left"]
POSITIONAL_SOURCE = "assets/positional/switch_buttons_{}_outline.png"

# And the shoulders, in GlyphCell order.
SHOULDERS = ["switch_button_l", "switch_button_r", "controller_button_l1",
             "controller_button_r1", "xbox_lb", "xbox_rb"]
SHOULDER_SOURCE = "assets/shoulders/{}.png"


def read_png(path):
    data = open(path, "rb").read()
    pos, idat, w, h = 8, b"", 0, 0
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            w, h, depth, colour = struct.unpack(">IIBB", body[:10])
            if depth != 8 or colour != 6:
                sys.exit("expected 8-bit RGBA")
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    raw, out, prev, p = zlib.decompress(idat), bytearray(), bytearray(w * 4), 0
    for _ in range(h):
        filt, p = raw[p], p + 1
        line = bytearray(raw[p:p + w * 4]); p += w * 4
        for i in range(w * 4):
            a = line[i - 4] if i >= 4 else 0
            b = prev[i]
            c = prev[i - 4] if i >= 4 else 0
            if filt == 1: line[i] = (line[i] + a) & 255
            elif filt == 2: line[i] = (line[i] + b) & 255
            elif filt == 3: line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif filt == 4:
                q = a + b - c
                pa, pb, pc = abs(q - a), abs(q - b), abs(q - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out += line; prev = line
    return w, h, bytes(out)


def read_palette_png_alpha(path):
    """The alpha of an indexed PNG (colour type 3, any bit depth), per pixel."""
    data = open(path, "rb").read()
    pos, idat, w, h, depth, trns = 8, b"", 0, 0, 8, b""
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            w, h, depth, colour = struct.unpack(">IIBB", body[:10])
            if colour != 3:
                sys.exit(f"{path}: expected an indexed PNG")
        elif kind == b"tRNS":
            trns = body
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    stride = (w * depth + 7) // 8
    raw, prev, p, alpha = zlib.decompress(idat), bytearray(stride), 0, []
    for _ in range(h):
        filt, p = raw[p], p + 1
        line = bytearray(raw[p:p + stride]); p += stride
        for i in range(stride):
            a = line[i - 1] if i >= 1 else 0
            b = prev[i]
            c = prev[i - 1] if i >= 1 else 0
            if filt == 1: line[i] = (line[i] + a) & 255
            elif filt == 2: line[i] = (line[i] + b) & 255
            elif filt == 3: line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif filt == 4:
                q = a + b - c
                pa, pb, pc = abs(q - a), abs(q - b), abs(q - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        per = 8 // depth
        row = []
        for x in range(w):
            byte = line[x // per]
            index = (byte >> (8 - depth * (x % per + 1))) & ((1 << depth) - 1)
            row.append(trns[index] if index < len(trns) else 255)
        alpha.append(row)
    return w, h, alpha


def render_positional(direction):
    """One positional cell: the source's alpha, fitted to the button's disc."""
    return render_fitted(POSITIONAL_SOURCE.format(direction), square=True)


def render_shoulder(name):
    """One shoulder cell: the source's alpha, as wide as the disc, sitting on
    the disc's bottom."""
    return render_fitted(SHOULDER_SOURCE.format(name), square=False)


def render_fitted(path, square):
    sw, sh, alpha = read_palette_png_alpha(path)

    def sample(sx, sy):
        # Bilinear, clamped, in source pixels.
        sx, sy = sx - 0.5, sy - 0.5
        x0, y0 = math.floor(sx), math.floor(sy)
        fx, fy = sx - x0, sy - y0
        def at(x, y):
            return alpha[min(max(y, 0), sh - 1)][min(max(x, 0), sw - 1)]
        top = at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx
        bottom = at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx
        return top * (1 - fy) + bottom * fy

    # The dots' own extent, not the image's: the source leaves a margin
    # around them, and a prompt drawn at text size has none to spare. The
    # extent is square -- the four dots make a diamond -- so fitting it to the
    # disc's square keeps the dots round.
    xs = [x for y in range(sh) for x in range(sw) if alpha[y][x] > 0]
    ys = [y for y in range(sh) for x in range(sw) if alpha[y][x] > 0]
    ex0, ex1, ey0, ey1 = min(xs), max(xs) + 1, min(ys), max(ys) + 1
    # A little room kept round them all the same, so the smoothing a prompt
    # is drawn with never reaches into the next cell of the strip.
    extent = max(ex1 - ex0, ey1 - ey0) * 1.08
    ecx, ecy = (ex0 + ex1) / 2.0, (ey0 + ey1) / 2.0
    if not square:
        # A shoulder is wider than it is tall: as wide as the disc, and
        # standing on the disc's bottom edge rather than centred in it.
        extent = (ex1 - ex0) * 1.12
        ecy = ey1 - extent / 2.0 + extent * 0.02

    cell = bytearray(CELL_W * CELL_H * 4)
    left, top, size = CX - R, CY - R, 2 * R
    for py in range(CELL_H):
        for px in range(CELL_W):
            total = 0.0
            for sy in range(SS):
                for sx in range(SS):
                    u = (px + (sx + 0.5) / SS - left) / size - 0.5
                    v = (py + (sy + 0.5) / SS - top) / size - 0.5
                    if -0.5 <= u < 0.5 and -0.5 <= v < 0.5:
                        total += sample(ecx + u * extent, ecy + v * extent)
            cell[(py * CELL_W + px) * 4 + 3] = round(total / (SS * SS))
    return bytes(cell)


def write_png(path, w, h, rgba):
    raw = b"".join(b"\x00" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    def chunk(kind, body):
        return (struct.pack(">I", len(body)) + kind + body +
                struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF))
    open(path, "wb").write(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b""))


def near_segment(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    length = dx * dx + dy * dy
    t = 0.0 if length == 0 else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / length))
    return math.hypot(px - (ax + dx * t), py - (ay + dy * t))


def in_symbol(name, x, y):
    """x, y relative to the disc's centre. True where the disc is cut away."""
    half = STROKE * 0.5
    if name == "cross":
        s = SPAN - 3.0
        return (near_segment(x, y, -s, -s, s, s) <= half or
                near_segment(x, y, -s, s, s, -s) <= half)
    if name == "circle":
        return abs(math.hypot(x, y) - (SPAN - 2.5)) <= half
    if name == "triangle":
        top, base, wide = -SPAN + 1.0, SPAN - 3.5, SPAN - 1.5
        return (near_segment(x, y, 0, top, -wide, base) <= half or
                near_segment(x, y, 0, top, wide, base) <= half or
                near_segment(x, y, -wide, base, wide, base) <= half)
    if name == "square":
        s = SPAN - 3.0
        return (near_segment(x, y, -s, -s, s, -s) <= half or
                near_segment(x, y, -s, s, s, s) <= half or
                near_segment(x, y, -s, -s, -s, s) <= half or
                near_segment(x, y, s, -s, s, s) <= half)
    if name == "bars":
        # Three horizontal lines: an Xbox Menu button and a PlayStation
        # Options button are the same icon.
        s, thick = SPAN - 1.5, 2.1
        return any(abs(y - offset) <= thick and abs(x) <= s for offset in (-8.0, 0.0, 8.0))
    if name == "view":
        # Two panes, one behind the other. Outlines were tried first and are
        # unreadable at this size: 27px across leaves a 3px stroke with 3px
        # of white inside it, and the two frames close up into a smudge.
        # Filled and set apart, the shape survives being small.
        def pane(x0, y0, x1, y1):
            return x0 <= x <= x1 and y0 <= y <= y1
        return pane(-11.0, -8.5, -1.5, -0.5) or pane(1.5, 0.5, 11.0, 8.5)
    if name == "create":
        # A rounded rectangle with a line down it.
        w, h = SPAN - 2.0, 7.5
        return (near_segment(x, y, -w, -h, w, -h) <= 2.0 or
                near_segment(x, y, -w, h, w, h) <= 2.0 or
                near_segment(x, y, -w, -h, -w, h) <= 2.0 or
                near_segment(x, y, w, -h, w, h) <= 2.0 or
                near_segment(x, y, -w * 0.25, -h, -w * 0.25, h) <= 2.0)
    sys.exit("unknown symbol " + name)


def render(name):
    cell = bytearray(CELL_W * CELL_H * 4)
    for py in range(CELL_H):
        for px in range(CELL_W):
            hits = 0
            for sy in range(SS):
                for sx in range(SS):
                    fx = px + (sx + 0.5) / SS
                    fy = py + (sy + 0.5) / SS
                    if math.hypot(fx - CX, fy - CY) <= R and not in_symbol(name, fx - CX, fy - CY):
                        hits += 1
            index = (py * CELL_W + px) * 4
            cell[index + 3] = round(255 * hits / (SS * SS))
    return bytes(cell)


def main():
    source = "assets/button_icons.png"
    w, h, rgba = read_png(source)
    if h != CELL_H:
        sys.exit(f"{source} is {h}px tall, expected {CELL_H}")
    kept = 8  # the hand-drawn Nintendo cells
    total = kept + len(GENERATED) + len(POSITIONAL) + len(SHOULDERS)
    out = bytearray(total * CELL_W * CELL_H * 4)
    for y in range(CELL_H):
        src = (y * w + 0) * 4
        dst = (y * total * CELL_W) * 4
        out[dst:dst + kept * CELL_W * 4] = rgba[src:src + kept * CELL_W * 4]
    cells = ([render(name) for name in GENERATED] + [render_positional(d) for d in POSITIONAL] +
             [render_shoulder(name) for name in SHOULDERS])
    for index, cell in enumerate(cells):
        column = kept + index
        for y in range(CELL_H):
            dst = (y * total * CELL_W + column * CELL_W) * 4
            out[dst:dst + CELL_W * 4] = cell[y * CELL_W * 4:(y + 1) * CELL_W * 4]
    write_png(source, total * CELL_W, CELL_H, bytes(out))
    print(f"{source}: {kept} kept + {total - kept} generated = {total} cells, "
          f"{total * CELL_W}x{CELL_H}")


if __name__ == "__main__":
    main()
