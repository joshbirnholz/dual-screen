#!/usr/bin/env python3
"""Builds assets/inter.ttf, the UI font, from Inter as Fontsource publishes it.

    pip install fonttools brotli
    python3 assets/make_ui_font.py path/to/inter-latin-500-normal.woff

The input is the Latin subset of Inter Medium from the @fontsource/inter
package on npm (5.3.0 when this was written): already cut down to ASCII and
Latin-1, which is everything the shell draws -- see src/render/font.cpp.

Two things are done to it, neither of which changes how a letter looks:

- It is unwrapped from WOFF into a plain TrueType file, which is what
  stb_truetype reads.
- Its kerning is taken out of the "extension" lookups it is stored in. Those
  are only a way of addressing a big table, and stb_truetype does not follow
  them -- so without this, pairs like "AV" and "To" are simply not kerned.

Inter is under the SIL Open Font License 1.1; see assets/inter-OFL.txt.
"""

import os
import sys

from fontTools.ttLib import TTFont

EXTENSION = 9


def unwrap_extension_lookups(font):
    lookups = font["GPOS"].table.LookupList.Lookup
    for lookup in lookups:
        if lookup.LookupType != EXTENSION:
            continue
        inner = [subtable.ExtSubTable for subtable in lookup.SubTable]
        lookup.LookupType = inner[0].LookupType
        lookup.SubTable = inner
        lookup.SubTableCount = len(inner)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    font = TTFont(sys.argv[1])
    font.flavor = None
    if "GPOS" in font:
        unwrap_extension_lookups(font)
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "inter.ttf")
    font.save(out)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
