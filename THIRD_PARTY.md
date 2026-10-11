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

## `third_party/stb/stb_image.h`, `stb_image_write.h`, `stb_image_resize2.h`

Sean Barrett's stb libraries, vendored verbatim from https://github.com/nothings/stb
(commit 2c980bb): reading the PNG and JPEG pictures a forwarder's icon is made
from, scaling them, and writing the JPEG the HOME menu shows. Public domain
(or MIT, at your choice) — the notice is at the end of each file.

## SDL2

A build and runtime dependency, not vendored. zlib licensed.

## melonDS DS

**Not** vendored, not linked at build time, and not redistributed. The core is
`dlopen`'d at runtime from wherever the user installed it, across the stable
libretro C ABI. melonDS DS is GPL-3.0; nothing from it is compiled into this
project.

## A DS core packaged in the Android build

Still `dlopen`'d at run time and still not linked, but no longer only
installed by the user: an Android build fetches melonDS DS (libretro's own
buildbot build of it, GPL-3.0) when it is built and packages it in the APK
beside the app's own library (see `android/README.md`). Nothing from it is
compiled into this project, and it is not in this repository -- it is
downloaded into `android/app/jni/melonds-ds/`, which git ignores.

**An APK built that way carries a GPL-3.0 program inside it.** Passing one on
is distributing melonDS DS, on the GPL's terms: with its source, or a written
offer of it. An APK for your own device, not passed on, raises none of this;
`-PnoDsCore` builds one with no core in it at all.

## A DS core linked into the Switch build

The one build that links a core rather than loading it. A Switch cannot load code at run time, so a
Switch build that plays DS games itself has its core -- libretro's melonDS, or
RSDuck's Switch port of melonDS, both GPL-3.0 -- linked into `Dualscreen.nro`
(see `switch/README.md`). melonDS itself is not in this repository: it is built
separately by whoever builds the Switch app, from their own clone, and
`switch/Makefile` links it only if it is there.

### `switch/melonds_deko/` -- GPL-3.0-or-later

The glue that makes RSDuck's melonDS a libretro core: the libretro interface,
and the platform and GPU setup its own Switch frontend otherwise provides. It
is written against melonDS's internal interfaces and exists only to be linked
with it, so it is licensed as melonDS is, GPL-3.0-or-later, as each of its
files says -- unlike the rest of this repository. It contains no code copied
from melonDS; it is compiled and linked only when the Switch app is built with
that core.

**That `.nro` is a combined work with a GPL-3.0 program, and distributing it is
distributing that combination.** Anyone who shares a Switch build with a core in
it has to do so on the GPL's terms, including making the corresponding source
available -- this project's as well as the core's. This repository carries no
licence of its own yet, which makes that impossible to do properly until it
does. A build for your own console, not passed on, raises none of this. A Switch
build without a core is the companion alone and links nothing of melonDS.

### `switch/forwarder/` -- GPL-3.0-or-later

What the Switch's **Create Forwarder** packs and installs a forwarder with:
`forwarder_nca.cpp` builds the program, control and meta NCAs and
`forwarder_install.cpp` writes them to the SD card's content storage and gives
the HOME menu the title. Both are adapted from Sphaira's `owo.cpp` and its
`yati/nx` helpers (https://github.com/ITotalJustice/sphaira, at commit
72a94b9 of the NaGaa95 fork; GPL-3.0; Sphaira's romfs building is itself after
hacbrewpack's), so they are GPL-3.0-or-later, as each file says. Compiled only
into a Switch build with a DS core, which is already the combined work
described above; `tests/test_forwarder_nca.cpp` links `forwarder_nca.cpp` to
test it and is never distributed.

### `switch/forwarder/hbl/` -- ISC

The hbloader every forwarder carries, built by its own `Makefile` and embedded
in the `.nro`: nx-hbloader (Copyright 2017-2018 nx-hbloader Authors, ISC
licensed, the notice in `switch/forwarder/hbl/LICENSE.md`) as Sphaira
modified it. `source/` and `hbl.json` are Sphaira's `hbl/`, unchanged.

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

## Kenney's Input Prompts — `assets/positional/`, `assets/shoulders/`, `assets/ui_icons/steamdeck.png`, `assets/ui_icons/wiiu_gamepad.png`

Button and device artwork by Kenney (www.kenney.nl), Input Prompts 1.5A,
released under Creative Commons Zero (CC0): the positional face-button dots,
the shoulder buttons (a Switch's L and R, a Steam Controller's L1 and R1, an
Xbox pad's LB and RB), the Steam Deck shown on Data Management's game
page, and the solid Wii U GamePad on Controller Order. Only their shapes are used — see
`assets/make_button_icons.py` and `assets/make_ui_icons.py`.

## Phosphor Icons — `assets/ui_icons/pc.svg`, `assets/ui_icons/pc.png`

`desktop-tower-fill`, the PC on Data Management's game page. Copyright (c)
2020 Phosphor Icons, MIT licensed.

## The Android robot — drawn in `assets/make_ui_icons.py`

The Android device on Data Management's game page is the robot's head, drawn
by that script rather than copied. The Android robot is reproduced or modified
from work created and shared by Google and used according to terms described
in the Creative Commons 3.0 Attribution License.
