# Third-party code

## `third_party/libretro/libretro.h`

The libretro API header, vendored verbatim from the RetroArch project so the
frontend implemented here is compiled against exactly the ABI the cores were
built for. Copyright (C) 2010-2024 The libretro team, MIT licensed — the full
notice is at the top of the file.

## nlohmann::json

Used for the config files. Not vendored: CMake prefers a system-installed copy
and only fetches the release archive if none is found. MIT licensed.

## SDL2

A build and runtime dependency, not vendored. zlib licensed.

## melonDS DS

**Not** vendored, not linked at build time, and not redistributed. The core is
`dlopen`'d at runtime from wherever the user installed it, across the stable
libretro C ABI. melonDS DS is GPL-3.0; nothing from it is compiled into this
project.

## `assets/ds_bezel.png` and `assets/three_ds_bezel.png`

Illustrations of a Nintendo DS and a Nintendo 3DS, drawn by the Standard layout
behind the two screens. Both were supplied by this project's author rather than
produced here, and are embedded into the binary at build time.

**Their provenance is not recorded, and it needs to be.** They depict Nintendo
hardware, and whoever made each illustration holds copyright in it independently
of that. Before this repository is published or a build is distributed, the
source and licence of these files should be established and written down here —
or the files replaced with ones whose terms are known.

## `assets/inter.ttf`

The UI font: [Inter](https://rsms.me/inter/) SemiBold by Rasmus Andersson,
version 5.3, taken from the `@fontsource/inter` package's Latin subset and
embedded into the binary at build time. Licensed under the SIL Open Font
License 1.1, whose full text travels with it as `assets/inter-OFL.txt`.
`assets/make_ui_font.py` records how the file was made from the published one:
unwrapped from WOFF, and its kerning moved out of the extension lookups that
stb_truetype does not read. No glyph is changed.

## What is deliberately absent

No Nintendo BIOS, firmware, NAND image, ROM, or font is included here, and none
is downloaded. BIOS and firmware files have to be supplied by the user; the app
locates and validates them and reports what is missing.
