# Third-party code

## `third_party/libretro/libretro.h`

The libretro API header, vendored verbatim from the RetroArch project so the
frontend implemented here is compiled against exactly the ABI the cores were
built for. Copyright (C) 2010-2024 The libretro team, MIT licensed — the full
notice is at the top of the file.

## `third_party/nlohmann/json.hpp`

Used for the config files. Release 3.11.3, the single-header distribution,
vendored verbatim for the Switch build, which has no package to find it in.
CMake still prefers a system-installed copy and uses this one otherwise.
Copyright (C) 2013-2023 Niels Lohmann, MIT licensed — the licence is beside it
in `third_party/nlohmann/LICENSE.MIT`.

## SDL2

A build and runtime dependency, not vendored. zlib licensed.

## melonDS DS

**Not** vendored, not linked at build time, and not redistributed. The core is
`dlopen`'d at runtime from wherever the user installed it, across the stable
libretro C ABI. melonDS DS is GPL-3.0; nothing from it is compiled into this
project.

## A DS core linked into the Switch build

The one exception to the above. A Switch cannot load code at run time, so a
Switch build that plays DS games itself has its core -- libretro's melonDS,
GPL-3.0 -- linked into `Dualscreen.nro` (see `switch/README.md`). Nothing from
it is in this repository: the core is built separately by whoever builds the
Switch app, and `switch/Makefile` links it only if it is there.

**That `.nro` is a combined work with a GPL-3.0 program, and distributing it is
distributing that combination.** Anyone who shares a Switch build with a core in
it has to do so on the GPL's terms, including making the corresponding source
available -- this project's as well as the core's. This repository carries no
licence of its own yet, which makes that impossible to do properly until it
does. A build for your own console, not passed on, raises none of this. A Switch
build without a core is the companion alone and links nothing of melonDS.

## `assets/ds_bezel.png` and `assets/three_ds_bezel.png`

Illustrations of a Nintendo DS and a Nintendo 3DS, drawn by the Standard layout
behind the two screens. Both were supplied by this project's author rather than
produced here, and are embedded into the binary at build time.

**Their provenance is not recorded, and it needs to be.** They depict Nintendo
hardware, and whoever made each illustration holds copyright in it independently
of that. Before this repository is published or a build is distributed, the
source and licence of these files should be established and written down here —
or the files replaced with ones whose terms are known.

## `assets/stylus_hand.png`

The symbol the Wii U Virtual Console's Screen Settings shows for which hand
holds the stylus: Nintendo's artwork, copied out of a screenshot of that
screen supplied by this project's author and made square by
`assets/make_stylus_hand.py`, which records exactly how. The same caution as
the bezel illustrations above applies before anything is published or
distributed.

## `assets/inter.ttf`

The UI font: [Inter](https://rsms.me/inter/) Medium by Rasmus Andersson,
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
