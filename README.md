# dualscreen

A shell for playing dual-screen games on Linux/SteamOS that treats each screen
as its own addressable source, so you pick a screen layout from a visual menu
instead of cycling a button — and the choice is remembered per game.

This is **V1**: one device, DS games, no networking. It exists to prove the
rendering/layout/persistence core and to put the seams in place that the later
phases (a networked host/companion split, 3DS, Wii U) plug into.

![the layout picker](docs/images/layout-picker.png)

## What it does today

- Embeds the **melonDS DS** libretro core directly — `dlopen`'d as a library,
  with the libretro frontend implemented here. RetroArch is not involved and
  does not need to be installed.
- Pins the core to a single known internal layout and splits its framebuffer
  into two textures — top screen and bottom screen — every frame.
- Composites those two textures into **your chosen layout**, which is entirely
  independent of whatever the core is doing internally.
- Runs as one fullscreen window, so it behaves like any other non-Steam
  shortcut under gamescope / Game Mode.
- Controller input via SDL2's GameController API; touch input on whichever
  screen is currently showing the bottom screen, mapped back into DS
  touchscreen coordinates.
- Remembers the layout and screen-swap flag per game, keyed by the ROM's SHA-1
  rather than its path, so moving your ROMs around loses nothing.

## Layouts

| id | what it shows |
| --- | --- |
| `vertical` | top screen above, bottom screen below (the DS's own arrangement) |
| `vertical_swapped` | bottom screen above, top screen below |
| `horizontal` | side by side, top screen on the left |
| `horizontal_swapped` | side by side, bottom screen on the left |
| `top_only` | top screen filling the window |
| `bottom_only` | bottom screen filling the window |

Adding a layout is meant to be adding data, not code: a `Layout` is a list of
normalized rects with a screen id each (`src/layout/layout_registry.cpp`).
Touch mapping is derived from the active layout's bottom-screen placement, so a
new layout gets working touch for free.

The two single-screen layouts are also the base case the networked companion
mode will reuse later: each device renders one screen full-window.

## Building

Needs a C++17 compiler, CMake 3.16+, SDL2 and nlohmann::json.

```sh
# Debian/Ubuntu
sudo apt install build-essential cmake libsdl2-dev nlohmann-json3-dev

# SteamOS / Arch
sudo pacman -S base-devel cmake sdl2 nlohmann-json
```

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build --output-on-failure
```

If nlohmann::json is not installed, CMake fetches it.

On SteamOS the read-only root filesystem makes installing build dependencies
awkward; building inside a `distrobox`/`toolbox` container and copying the
resulting binary out is the path of least resistance.

## What you need to supply

**The core.** Install melonDS DS (`melondsds_libretro.so`) — through
RetroArch's core downloader, your distribution, or a release from the melonDS
DS project. dualscreen looks in the usual RetroArch core directories and
`/usr/lib/libretro`; override with `--core` or the `core_path` setting.

**BIOS/firmware, if you want them.** melonDS DS boots DS games using its own
open-source BIOS replacement, so nothing is strictly required. For native boot
or DSi mode, put your own dumps (`bios7.bin`, `bios9.bin`, `firmware.bin`, and
the `dsi_*` files) in the system folder. **No Nintendo-owned data ships with
this app and none is downloaded** — these have to be your own. The Settings
screen shows exactly which files it can and cannot see.

**ROMs.** Yours, obviously.

## Running

```sh
dualscreen --game ~/roms/nds/game.nds
```

That boots straight into the game with its last-used layout — no launcher in
the way. Full options:

```
--game <path>        ROM to boot straight into
--core <path>        melonDS DS libretro core (.so) to load
--layout <id>        Override the layout for this run
--system-dir <path>  Folder holding your BIOS/firmware files
--windowed           Run in a window instead of fullscreen
--list-layouts       Print the available layout ids and exit
--verbose            Log core and frontend debug output
```

### In-game controls

| | |
| --- | --- |
| Open/close the layout menu | **L3**, **Select+Start**, or a **two-finger tap** |
| Touch the DS touchscreen | tap or drag on whichever screen is showing the bottom screen |
| Move around the menus | d-pad or left stick; **A** confirms, **B** goes back |

The two-finger tap matters: on a tablet with no buttons attached, it is the
only way into the menu. Emulation pauses while the menu is open, so you can
compare layouts against the frame you were actually looking at.

### Adding a Steam shortcut

Add `dualscreen` as a non-Steam game, then set the launch options to
`--game "/path/to/rom.nds"` — one shortcut per game. Each one boots into its
own remembered layout, which is the whole point.

V1 does not script `shortcuts.vdf` for you.

## Where things are kept

```
~/.config/dualscreen/settings.json          app-level defaults
~/.config/dualscreen/games/<rom-sha1>.json  per-game layout + screen swap
~/.local/share/dualscreen/saves/            cartridge saves (the core writes these)
~/.local/share/dualscreen/system/           where BIOS/firmware files are looked for
```

`settings.json`:

```json
{
  "default_layout_id": "vertical",
  "default_screen_swap": false,
  "system_dir": "/home/deck/.local/share/dualscreen/system",
  "core_path": "",
  "stick_deadzone": 0.35,
  "trigger_deadzone": 0.3,
  "vsync": true
}
```

Paths are edited here rather than in the app: an on-screen keyboard is not
something V1 builds. The Settings screen cycles between BIOS folders that
actually exist and reports what it finds in them.

## How it fits together

```
LibretroCoreScreenSource ──► two textures (top, bottom) ──► Compositor ──► window
        │                          per frame                    │
        │                                                  OutputRouter
   LibretroCore                                        (local vs. elsewhere)
   (dlopen'd melonDS DS,                                       │
    pinned to one internal layout)                       InputManager
        ▲                                                       │
        └──────────────── CoreInputSink ◄───── DS buttons + touch in DS space
```

Three seams exist so the later phases are additive rather than a rewrite:

- **`ScreenSource`** (`src/core/screen_source.hpp`) produces two textures per
  frame plus their native resolution. V1's only implementation wraps the
  libretro core; a 3DS source and a Wii U window-capture source drop in here
  without the compositor or the layout system knowing.
- **`InputSink`** (`src/input/ds_input.hpp`) accepts DS button state and a
  touch point already in DS coordinate space. V1 fills it from local SDL
  events; a network-delivered companion input stream submits through the same
  interface.
- **`OutputRouter`** (`src/render/compositor.hpp`) is the single decision point
  for whether a screen is drawn locally or handed off. V1 always answers
  "locally". Sending one screen to a companion device becomes a swap-in here,
  not a refactor spread through the renderer.

The per-game config already keys on `rom_sha1` and reserves `role_default` and
`companion_device_id`, so V1's files will not need a migration.

## Tests

```sh
ctest --test-dir build --output-on-failure
```

- `test_layout` — placement geometry, screen swap, and touch mapping round-trips
  for every layout in both swap states.
- `test_config` — SHA-1 against the FIPS vectors, path handling, config
  round-trips including preservation of keys a future version might add.
- `test_core_host` — loads a **mock libretro core** (`tests/mock_core`) and
  checks option forcing, geometry, video capture and the frontend's own
  validation of what the core accepted.
- `smoke_headless` — runs the real binary against the mock core under `Xvfb`,
  screenshots every layout, and checks pixel by pixel that each screen landed
  where the layout says. It also holds a synthetic touch and verifies the
  crosshair the core draws comes back **exactly under the touch point** — which
  only happens if the whole window → DS → pointer → framebuffer → split →
  composite path agrees.

The mock core means all of this runs with no melonDS core and no commercial ROM.

## Not in V1

Networking, host/companion split, 3DS, Wii U, Android or Switch builds, save
states, cheats, rewind, shaders. See `docs/research-notes.md` for what was
verified against the real melonDS DS core and what still has to be checked on
hardware.
