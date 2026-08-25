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
| Open this device's menu | **+ and -** together, or a **two-finger tap** |
| Move around that menu | d-pad or left stick; **A** changes a row, **B** resumes |
| Smooth or sharpen the screens | **R3** — the host applies it on both devices |
| Swap the two focus layouts | **L3** — likewise |
| Touch the touchscreen | tap or drag, when this device is the bottom screen |

The in-session menu carries the whole session, not a cut-down version of it: a
player holding this device is holding the controller, and reaching for a second
pad plugged into the machine across the room to change the layout is the thing
it exists to avoid. **Layout**, **Smoothing**, **Render**, **Right Stick**,
**Hands**, **Gap** and **Sound**, then **Reset Game**, **Resume**,
**Disconnect** (leave, and go back to looking for a host) and **Quit** (end the
session on both devices and close the app, the same as choosing Quit on the
other one).

Up and Down move; Left and Right cycle a row's value and A does the same; B
resumes. Rows that mean nothing right now are simply absent — no **Render** on
a console whose core draws at one size, no **Gap** on a layout whose gap is not
a choice — the same rule the host's own menu follows.

Every one of them is a *request*. This device applies none of it: the host owns
the session, decides, and broadcasts the result, which is what keeps the two
ends from ever disagreeing. A change made on the host shows up here too, under
the open menu.

## Building

Needs devkitPro with the Switch toolchain and two portlibs:

```sh
sudo dkp-pacman -S switch-dev switch-sdl2 switch-zlib
export DEVKITPRO=/opt/devkitpro
cd switch && make
```

That produces `dualscreen-companion.nro`. Copy it to `/switch/` on the console's
SD card and start it from the homebrew menu.

A finished `.nro` carries an asset section on the end — `icon.jpg` and the name
and version from the top of the `Makefile` — so a menu shows it as *dualscreen
companion* with the two-screen icon rather than as a bare filename. If a loader
says it **cannot parse the nro**, that section is what it is missing.

## Reading the log

The app writes everything it does to the SD card, at

```
/switch/dualscreen-companion.log
```

A fresh one per run, flushed a line at a time, so it survives the app being
killed rather than exiting. Pull the card, or read it over the network with
`nxmtp`/`ftpd`.

For the same lines as they happen, run `nxlink -s` from a computer on the same
network — the app calls `nxlinkStdio()` at start-up, so its output arrives
there. That needs the console to already be listening (`nxlink` is launched
from the homebrew menu's own network loader), which is why the file exists too.

## If it exits straight back to the homebrew menu

**No log file on the SD card at all** means the app never ran: the log is
opened before anything else can fail, so its absence is not a clue about what
the app did, it is the answer that nothing did. That is a loader refusing the
`.nro` — the same thing Sphaira reports as "cannot parse nro" — and it is a
packaging problem rather than a running one. Rebuild after `make clean` and
check the asset section is there, as above.

**A log file that stops part-way** is the app deciding it cannot start, which
is not a crash — a crash gets a fatal error screen from the console. It says
why on screen before it goes, and the last line of the log is where it got to.

The usual cause there is **applet mode**. Launching homebrew from the album gives it
about 50 MB of heap, which is not enough for SDL and the graphics stack behind
it, and the failure comes out as a window or renderer that will not open. Start
the homebrew menu in full-memory mode instead: hold **R** while launching a
game from the console's home screen, and run the app from the menu that opens.

Sound is not a reason to refuse to start: if the audio device will not open the
log says so and the app carries on without it.

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
