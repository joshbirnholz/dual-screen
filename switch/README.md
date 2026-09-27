# Dualscreen for the Nintendo Switch

A Switch homebrew app that joins a game already running on another device and
becomes one of its two screens.

Built as it comes, it runs no emulator and holds no ROM. There is no core to
install, no BIOS to supply and nothing to dump: everything is being played on
the other device, and this app draws the screen it was given and sends its
buttons and touches back.

A Switch can play DS games perfectly well itself, though (a 3DS or a Wii U it
cannot), so the app can also be built with a DS emulator in it, and then it
plays DS games on the console too. See **Playing DS games on the Switch**
below.

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
- Stays awake while it is connected. The screen neither dims nor sleeps, so
  a Switch put down for a while is still there when it is picked up. It is
  allowed to sleep again once the session ends.
- Reports each controller attached to it separately, so on a
  Wii U the host can make them different players (see **Players** in the main
  README). Each is named by what it is -- "Pro Controller", "Left Joy-Con",
  "Switch Lite" for a Lite's own controls, "Joy-Con (attached)" for another
  Switch's -- and reports which player slot it is in, the number its own
  player lights show. A controller in player 1's slot and the handheld
  controls are two controllers, although SDL reads them as one. The Switch
  also tells the host which model it is, so the host calls it "Switch Lite"
  rather than "companion". When the host has Settings or Controller Order open,
  the Switch says so and its buttons work that screen.

Everything the two devices have to agree on — the layout, smoothing, the gap,
handedness, which device makes the sound — belongs to the host. This end asks
and then follows, exactly as the desktop companion does, which is what keeps
the two from ever disagreeing.

## Controls

| | |
| --- | --- |
| Choose which screen to be | the opening screen, with the d-pad or left stick |
| Move around the menus | d-pad or left stick |
| Join, and confirm in menus | the east face button (A) |
| Go back, or leave a menu | the south face button (B) |
| Open this device's menu | **L3 and R3** together, or a **two-finger tap** |
| Move around the menus | d-pad or left stick; the east face button confirms, the south one goes back |
| Smooth or sharpen the screens | **R3** alone — the host applies it on both devices |
| Swap the two focus layouts | **L3** alone — likewise |
| Touch the touchscreen | tap or drag, when this device is the bottom screen |

The in-session menu **is** the app's own quick menu — the same tile grid of
layouts with live previews drawn from the frame the game is paused on, the same
footer, the same entries in the same places. Not a port of it: the same source
file, compiled again. A player moving between the two devices should not have
to learn a second menu.

The one entry missing here is **Pointer**, because it is the only one that
belongs to the device rather than to the host, and a console has no arrow to
show or hide. Everything else on that menu is a *request*: this device applies
none of it, the host decides and broadcasts the result, which is what keeps the
two ends from ever disagreeing. A change made on the host appears here too,
under the open menu.

The opening screen and the host picker are the app's own too, for the same
reason. The opening screen is that menu with the two entries that mean nothing
here left out — this device has no ROMs to load, and nothing in Settings would
apply to it — so what is left is **Connect as top screen**, **Connect as bottom
screen** and **Quit**.

Picking a host puts up a screen that says so, naming the device and its game.
The list on its own looked identical whether or not the press had landed, and
with every button ignored until the handshake finishes, a host that is slow to
answer was indistinguishable from the button doing nothing. **B** stops
waiting; a host that never answers is given a few seconds and then says so.

## Motion

A Wii U GamePad has a gyroscope and an accelerometer, and a great many of its
games steer, aim or look around with them -- Splatoon, ZombiU, Mario Kart 8,
Wind Waker HD's telescope. Joined to a Wii U host, this device stands in for
that GamePad, so how it is being held is sent along with its buttons.

What crosses the wire is raw: rotation in radians per second and acceleration
in metres per second squared, exactly as the sensors report them, plus the
moment each reading was taken. Nothing on this side turns that into an
orientation. Cemu already does, with sensor fusion tuned to what a real
GamePad reports, and a second answer to the same question computed here would
only be a different one.

Only on a console that reads it. The host says whether it does, and until it
says so no sensor is switched on at all -- a gyroscope left running is
battery spent for nothing on a handheld. So a DS or 3DS session costs this
nothing, and a Wii U session starts the sensors when it connects and stops
them when it leaves.

The log says what it found, once, when a Wii U host asks:

```
motion: the host reads it; 1 of 1 connected controllers report it
motion: this device is reporting how it is being held
```

`0 of 1` is the line to look for when aiming does nothing. It means SDL cannot
see any sensors on the pad, which is almost never the pad's fault -- see the
device-specific note below.

### On this console

The six-axis unit is read through libnx rather than SDL: the Switch port's SDL
exposes the pads' buttons and sticks but not their sensors. Handheld mode and
a pair of joined Joy-Cons both work; a single Joy-Con held sideways does not,
which is the same limitation Nintendo's own games have.

The console must have its motion controls enabled in system settings. With
them off, `hidGetSixAxisSensorHandles` succeeds and reports nothing, which
comes out here as `nothing here reports how it is being held`.

## Building

Needs devkitPro with the Switch toolchain and two portlibs. On Windows, run
this from the MSYS2 shell devkitPro's own installer sets up, with no `sudo`
— there is no such command in that shell (what a `sudo` typed there actually
runs is Windows' own unrelated `sudo.exe`, which has nothing to do with
this), and the whole devkitPro tree is already owned by your own account
there. On Linux, prefix `pacman` with `sudo` if devkitPro was installed
somewhere only root can write. Plain `pacman` rather than `dkp-pacman`:
devkitPro's own docs say either should resolve to the same thing on a
properly set-up install, but only `pacman` is guaranteed to be on PATH — it
ships with MSYS2 itself, where `dkp-pacman` is a devkitPro-added wrapper
that is not always there to find:

```sh
pacman -S switch-dev switch-sdl2 switch-zlib
export DEVKITPRO=/opt/devkitpro
cd switch && make
```

That produces `Dualscreen.nro`. Copy it to `/switch/` on the console's
SD card and start it from the homebrew menu.

### H.264 video

Add `switch-ffmpeg` and the GamePad screen streams as H.264 through Averne's
NVTEGRA hwaccel instead of as changed tiles, at a fraction of the bandwidth:

```sh
pacman -S switch-ffmpeg
```

Optional, and detected automatically: `make` finds it or does not, and says
which in its own output. Nothing else changes — a host that cannot encode
video, or a companion the host is not sending it to, still gets the tile
codec exactly as before.

A finished `.nro` carries an asset section on the end — `icon.jpg` and the name
and version from the top of the `Makefile` — so a menu shows it as *dualscreen
companion* with the two-screen icon rather than as a bare filename. If a loader
says it **cannot parse the nro**, that section is what it is missing.

## Playing DS games on the Switch

With a DS core built in, the opening screen gains **Load a game**, and the app
plays DS games on the console with everything the desktop app has for them:
the same file browser, the same layouts and quick menu, per-game options, save
points, **Resume from Save State**, and a second device joining as the other
screen. It is the desktop app's own code, compiled for the Switch, not a port
of it.

**Load a game** starts Dualscreen again, straight into the file browser. It
opens at `/roms/nds` (or `/roms/NDS`, `/roms/ds`, `/roms`, `/ROMs`, whichever is
on the card first), else at the card's root. Leaving the browser, or quitting a
game from its quick menu, brings the opening screen back the same way.

Only DS games: a 3DS file is refused with a message saying so.

### A game's own forwarder

Dualscreen goes straight into a game when started with its ROM as an argument,
which is what makes a forwarder for one game possible. A forwarder that starts
an `.nro` with arguments should start

```
sdmc:/switch/Dualscreen.nro
```

with the ROM's path as the argument, plain or after `--game`:

```
sdmc:/roms/nds/Some Game.nds
--game "sdmc:/roms/nds/Some Game.nds"
```

Quitting a game started this way leaves Dualscreen, back to wherever the
forwarder was started from, as leaving any game does.

### Where it keeps things

Everything is under `/switch/dualscreen/` on the SD card:

| | |
| --- | --- |
| `saves/` | the game's own saves (`Some Game.srm`, named like RetroArch's), and save points |
| `games/` | per-game options and layouts |
| `system/` | BIOS and firmware, if the core wants any — Settings, in a game's quick menu, offers RetroArch's `/retroarch/cores/system` instead when it is on the card |
| `settings.json` | the app's settings |

A game played here logs to `/switch/Dualscreen-game.log`, beside the menu's own
log, because leaving the game starts the menu's log afresh.

### Building it in

The core is libretro's melonDS, built as a static library for the Switch. It
is GPL-3.0 and not part of this repository, so it is built separately and
dropped in:

From this `switch` directory — `cores/` is ignored by git, so the core and its
source stay out of this repository:

```sh
git clone https://github.com/libretro/melonDS cores/melonDS
make -C cores/melonDS platform=libnx AR=$DEVKITPRO/devkitA64/bin/aarch64-none-elf-ar -j8
cp cores/melonDS/melonds_libretro_libnx.a cores/
make
```

`AR=` with the whole path because of how its last step runs. devkitPro's
rules put the toolchain on the path for the commands `make` hands to a shell,
but a command with nothing in it for a shell to interpret -- the archiver line
-- is started directly, and the `make` that comes with macOS looks for it on the
path it was itself started with, which does not have devkitPro's tools on it.
Every compile succeeds and then `aarch64-none-elf-gcc-ar: No such file or
directory` at the end. Naming the archiver in full works everywhere, and plain
`ar` makes the same archive.

`make` looks for `cores/melonds_libretro_libnx.a` and says whether it found it.
A core anywhere else, or named differently, is given as
`make CORE_LIB=/path/to/core.a`. With no core, the build is the companion
alone, exactly as before.

**Speed.** While a game plays here the app runs the CPU at 1785 MHz rather
than the 1020 MHz programs get by default -- the speed the Switch's own games
use while loading -- and puts the threads melonDS, the sound and the network
start on cores of their own, where otherwise every one of them would share
the main thread's. It puts the clock back when the game ends. melonDS's own
Switch port (RSDuck's) did both.

That port also always ran melonDS with its **JIT**, which translates DS code
into the Switch's own instead of interpreting it one instruction at a time,
and most games need it to keep up. libretro's build leaves it switched off,
with a note about memory problems still to be solved, so it is not the
default here yet; `make` says whether the core it found has it. To build the
core with it on (the `clean` matters: every file compiles differently with
it):

```sh
make -C cores/melonDS platform=libnx clean
make -C cores/melonDS platform=libnx JIT_ARCH=aarch64 AR=$DEVKITPRO/devkitA64/bin/aarch64-none-elf-ar -j8
cp cores/melonDS/melonds_libretro_libnx.a cores/
make
```

The game log says how fast a game is running every ten seconds (lines
starting `speed:`), which is the way to tell whether it helped. If games crash
with it, build the core again without `JIT_ARCH`.

Keep the clone in `cores/melonDS` after copying the archive out: the link needs
its source as well. A core built to be linked into a program leaves out
libretro-common's file, path and string helpers (`rfopen`, `path_basename`,
`strlcpy_retro__` and the rest), because RetroArch has its own copy, so `make`
compiles them from the clone's `src/libretro/libretro-common`. A clone
somewhere else is given as `make CORE_SRC=/path/to/melonDS`.

**Before passing a built `.nro` on to anyone:** with melonDS linked in, it is a
combined work with a GPL-3.0 program — see `THIRD_PARTY.md`.

## Reading the log

The app writes everything it does to the SD card, at

```
/switch/Dualscreen.log
```

A fresh one per run, flushed a line at a time, so it survives the app being
killed rather than exiting. Pull the card, or read it over the network with
`nxmtp`/`ftpd`.

For the same lines as they happen, run `nxlink -s` from a computer on the same
network — the app calls `nxlinkStdio()` at start-up, so its output arrives
there. That needs the console to already be listening (`nxlink` is launched
from the homebrew menu's own network loader), which is why the file exists too.

## Docked and handheld

The console can hand SDL a window the size of its *docked* output even when it
is being held: undocked, the panel is 1280x720 and a 1080p picture is scaled
down onto it. So the window's size is not the size of the screen in front of
the player, and a layout that trusts it gets the handheld case wrong — it
keeps the margin that only a big display has room for.

The client asks the console instead. `appletGetOperationMode()` says handheld
or docked, and that decides the display size the layout is worked out
against: 1280x720 held, 1920x1080 docked. Drawing still fills whatever window
there is; it is only the question of *how much room there really is* that
follows the panel. That is what makes the Standard layout fill a handheld
screen with the console's own screens, cropping the artwork around them,
rather than sitting the whole picture in a field of grey — the same rule a
Deck gets.

It also caps what is worth streaming: the host is asked for frames at the
panel's size, so nothing is sent that the panel would only throw away.

The log says which it is on every change (`the panel is 1280x720`), and says
the window size separately (`drawing at 1920x1080`). The two disagreeing is
normal, and those two lines are the first thing to check if a layout looks
the wrong size.

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

Nearly all of it is the desktop app's own sources, compiled again: the wire
protocol, LAN discovery, the session, the layout arithmetic, the controller
mapping, the compositor that draws the screens, the quick menu and the host
picker. There is one implementation of each rather than a second to keep in
step, which is why this directory holds no source at all — only the build.

The client itself is `../companion/source/main.cpp`, one program for three
devices: this console, an Android handheld (see
[../android/README.md](../android/README.md)) and a desktop. Only start-up,
shutdown and how a device describes its own screen differ between them.

That is deliberate down to the pixels. Every rule about how a screen is drawn
— the margin that drops away on a display no bigger than a Deck's, which a
Switch's 1280x720 is; the console artwork behind the Standard layout; when a
screen is smoothed and when it is left alone — is the one rule rather than two
that agree until one of them changes.

The client is one program with a single guarded section, in `source/main.cpp`,
for the console's own start-up and shutdown. Everything else builds and runs on
a desktop, so the root `CMakeLists.txt` builds it as `dualscreen-companion`
for this machine too, and `tests/run_switch_client_test.sh` runs it against a
real dualscreen host under a headless X server and checks it ends up showing
the screen it asked for. What it draws is covered by the tests the app's own
menu and compositor already have, since it is the same code.

The Switch build itself is not covered by that — it needs a toolchain no CI
here has — so `switch/Makefile` and the `__SWITCH__` sections of `main.cpp`
are the parts to check by hand after changing anything in this directory.
