# dualscreen companion for the Nintendo Switch

A Switch homebrew app that joins a game already running on another device and
becomes one of its two screens.

It runs no emulator and holds no ROM. There is no core to install, no BIOS to
supply and nothing to dump: everything is being played on the other device, and
this app draws the screen it was given and sends its buttons and touches back.
That is the whole of it.

## What it does

- Finds any device on the same network that is running a game in dualscreen,
  without anyone typing an address.
- Joins as the **top** or the **bottom** screen. Whichever it takes, the other
  device takes the other one.
- Draws whatever the host sends, arranged by the layout the host is using —
  including the layouts that put both screens on one device, which the host
  sends both screens for.
- Sends the pad back: buttons, the d-pad, both sticks, and the touchscreen when
  this device is holding the touch screen.
- Plays the game's sound, when the host's **Sound** setting says this device is
  one of the ones it comes out of.

Everything the two devices have to agree on — the layout, smoothing, the gap,
handedness, which device makes the sound — belongs to the host. This end asks
and then follows, exactly as the desktop companion does, which is what keeps
the two from ever disagreeing.

## Controls

| | |
| --- | --- |
| Move around the host list | d-pad or left stick |
| Choose top or bottom screen | Left / Right on the host list |
| Join, and confirm in menus | the east face button (A) |
| Leave the list, or a menu | the south face button (B) |
| Open this device's menu | a **two-finger tap** on the screen |
| Smooth or sharpen the screens | **R3** — the host applies it on both devices |
| Swap the two focus layouts | **L3** — likewise |
| Touch the touchscreen | tap or drag, when this device is the bottom screen |

The in-session menu is three entries: **Resume**, **Disconnect** (leave, and go
back to looking for a host) and **Quit** (end the session on both devices and
close the app, the same as choosing Quit on the other one).

## Building

Needs devkitPro with the Switch toolchain and two portlibs:

```sh
sudo dkp-pacman -S switch-dev switch-sdl2 switch-zlib
export DEVKITPRO=/opt/devkitpro
cd switch && make
```

That produces `dualscreen-companion.nro`. Copy it to `/switch/` on the console's
SD card and start it from the homebrew menu.

Run it over the network while developing and its log comes out on `nxlink`.

## How it is put together, and how it is tested

The parts that describe a session — the wire protocol, LAN discovery, the
session itself, the layout arithmetic and the controller mapping — are the
desktop app's own sources, compiled unchanged. There is one implementation of
each rather than a second one to keep in step with it, which is also why this
directory is so small: what is here is a window, a renderer, a host picker and
a short menu.

The client is one program with a single guarded section, in `source/main.cpp`,
for the console's own start-up and shutdown. Everything else builds and runs on
a desktop, so the root `CMakeLists.txt` builds it as `dualscreen-companion`
for this machine too, and `tests/run_switch_client_test.sh` runs it against a
real dualscreen host under a headless X server and checks it ends up showing
the screen it asked for. `tests/test_switch_client.cpp` covers the two pieces
of logic worth testing on their own: the host list as hosts come and go, and
where a quarter-turned screen actually has to be drawn.

The Switch build itself is not covered by that — it needs a toolchain no CI
here has — so `switch/Makefile` and the `__SWITCH__` sections of `main.cpp`
are the parts to check by hand after changing anything in this directory.
