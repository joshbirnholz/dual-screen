#!/usr/bin/env python3
"""Whether a screenshot (binary PPM, as --screenshot writes it) shows the red
screen tests/make_test_nds.py's program draws: a DS core really ran it.

usage: check_red_screen.py <shot.ppm>
"""

import sys


def main() -> int:
    data = open(sys.argv[1], "rb").read()
    parts = data.split(b"\n", 3)
    if parts[0] != b"P6":
        print(f"{sys.argv[1]} is not a binary PPM", file=sys.stderr)
        return 1
    width, height = map(int, parts[1].split())
    pixels = parts[3]
    red = total = 0
    for i in range(0, min(len(pixels), width * height * 3), 3 * 7):
        r, g, b = pixels[i], pixels[i + 1], pixels[i + 2]
        total += 1
        if r > 200 and g < 40 and b < 40:
            red += 1
    share = red / max(total, 1)
    print(f"{width}x{height}, {share:.1%} red")
    # A DS screen is a fraction of the window; a tenth of it red is plenty.
    return 0 if share > 0.05 else 1


if __name__ == "__main__":
    sys.exit(main())
