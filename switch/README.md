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

### SteamGridDB for forwarder icons

Add `switch-curl` and Create Forwarder (below) can search SteamGridDB for a
game's icon; without it, a forwarder's icon is the ROM's own:

```sh
pacman -S switch-curl
```

Optional and detected the same way as FFmpeg below.

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
on the card first), else at the card's root. **+** (Main Menu) leaves the
browser for the opening screen, from however deep in the folders it is; B goes
up one folder, and out once there is none. Quitting a game from its quick menu,
or closing it from the HOME menu, closes Dualscreen, as quitting any game does.

Only DS games: a 3DS file is refused with a message saying so.

### A game's own forwarder

A game can have its own icon on the HOME menu, which starts Dualscreen
straight into it. Highlight its ROM in the file browser and press **X**
(Create Forwarder) — the same thing Sphaira does for any `.nro`, with the
screen laid out the way Sphaira's is:

- **App Title** — what the HOME menu will call it. It starts as the name the
  ROM gives itself (the file's name if it gives none); select the box to type
  another.
- **Icon** — the ROM's own banner icon, enlarged; select it to choose between
  that and **SteamGridDB**, which searches [SteamGridDB](https://www.steamgriddb.com)
  by the title and shows the pictures it has for the game: square grids and
  icons first, then its tall and wide grids, whole, over a blur of themselves.
  The search is forgiving — `(USA)`, `[!]`, `™` and the like are left out, and
  a name SteamGridDB does not know is tried again shorter (without its
  subtitle, without a trailing "DS") — and when it still finds nothing, or
  the wrong game, **Y (Other Game)** lists the games the name matched and
  **Search for Another Name** takes whatever you type.
- **Publisher** — the publisher the ROM's banner names, to type over.
- **Screenshots** and **Video Capture** — whether the Capture button works in it.
- **Create Forwarder** — installs it. It will be on the HOME menu once it is
  done. Making one again for the same ROM replaces it.

SteamGridDB needs an API key, free from the site's
[preferences page](https://www.steamgriddb.com/profile/preferences/api) once
signed in. The first search will ask for it with the console's keyboard, and
it is kept in `settings.json` (`steamgriddb_api_key`) after that; a key the
site turns down is forgotten, and the next search will ask again. The
download checks the site's certificate against the certificates the console
itself trusts; nothing is fetched unverified.

A forwarder is an installed title, so it needs a console that runs unsigned
titles (Atmosphère), and it installs to the SD card. Each one starts
`hbloader` — built from `forwarder/hbl` into Dualscreen itself — which in
turn starts `Dualscreen.nro` from wherever it was when the forwarder was made,
with `--game` and the ROM's path. Move either, and make the forwarder again.
Delete one as any other software, from its Options (+) on the HOME menu.

A forwarder made some other way works too. Dualscreen goes straight into a
game when started with its ROM as an argument, so one that starts an `.nro`
with arguments should start

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
| `saves/` | the game's own saves (`Some Game.sav`, kept by melonDS itself), and save points |
| `games/` | per-game options and layouts |
| `system/` | BIOS and firmware, if the core wants any — Settings, in a game's quick menu, offers RetroArch's `/retroarch/cores/system` instead when it is on the card |
| `settings.json` | the app's settings |

A game played here logs to `/switch/Dualscreen-game.log`, beside the menu's own
log, because leaving the game starts the menu's log afresh.

### Building it in

The emulator is melonDS, GPL-3.0 and not part of this repository, so it is
built separately and linked in. Either of two builds of it can be: `make`
uses the first it finds in `cores/`, and says which.

#### RSDuck's melonDS, drawing on the GPU (the faster one)

RSDuck's Switch port of melonDS draws the DS's 2D and 3D on the Switch's GPU
through deko3d, where libretro's melonDS draws them in software on a CPU core
-- which a 3D game's races, Mario Kart DS's for one, are more than it can keep
up with. It is not a libretro core, so `melonds_deko/` here wraps it in the
libretro interface this app talks to; the emulator itself is compiled from
RSDuck's tree, unchanged but for the fixes in `melonds_deko/patches/`, which
the build applies to copies of the files they change (see below). (It is the
same emulator Gheovgos's melonDS for
Switch, release 7.x, is built on.)

From this `switch` directory — `cores/` is ignored by git:

```sh
pacman -S deko3d uam        # once: the GPU library and its shader compiler
git clone -b switch-new https://github.com/RSDuck/melonDS cores/melonds-deko
make -C melonds_deko -j8 && make
```

That makes `cores/melonds_deko_libnx.a` and `cores/melonds_deko_romfs`, the
shaders, which `make` builds into the `.nro`'s romfs. After the first time,
`make` alone is enough: while `cores/melonds-deko` is there it brings the core
up to date first, so a change to `melonds_deko/` is never left out of the app.

Two things it needs that libretro's melonDS does not:

- **A DS's BIOS and firmware.** It has no built-in stand-ins, and boots
  nothing without `bios7.bin`, `bios9.bin` and `firmware.bin`. They are looked
  for in `/switch/dualscreen/system/` and then `/switch/melonds/`, where
  melonDS's own Switch app keeps them -- so with that app set up, there is
  nothing more to do.
- **Memory.** Its GPU renderer wants about 390 MB of its own -- the same as
  melonDS's own Switch app gives it. Started from the album, a program gets
  too little for that; start Dualscreen through a game (hold R while starting
  one) instead. If there is not enough, the game says so rather than starting.

A few things about how it runs can be changed, for comparing a game here with
melonDS's own Switch app, in **Settings > melonDS**. Each takes effect the next
time a game starts. They are kept in `/switch/dualscreen/melonds.ini`, which
can be edited by hand as well -- Settings keeps any comments and lines of its
own -- and has a line for each one not at its default:

```ini
cpu_mhz=1020   # the CPU clock while playing; 1785 unless this says otherwise
jit=0          # the JIT off (most 3D games then run well below full speed)
fastmem=0      # the JIT's fast memory off
pipeline=0     # each frame shown as soon as it is drawn, not a frame later
gpu_alone=1    # the app's own drawing finished before each frame of the game
texcache=0     # the 3D renderer's texture cache emptied before every frame
shaders=standalone  # the GPU shaders from inside melonDS's own .nro
vramcheck=1    # video memory checked twice a second; differences logged
gpucache=0     # the renderers' GPU memory kept out of the GPU's L2 cache
l2flush=1      # each upload written out of the GPU's L2 cache at once
settle=2       # after each upload: 1 a small copy, 2 what it wrote read back
renderer=soft  # melonDS's software renderers draw everything, on the CPU
dump=300       # frames 300 to 307 after each load written to the SD card, raw
```

**The patches of garbage** -- 16x8 pixels, in Bowser's Inside Story's battle
menus and on Bowser in the overworld, in Mario Kart DS's track walls -- were
3D textures uploaded to the wrong place, and are fixed by
`melonds_deko/patches/GPU3D_Deko-texture-tile-size.patch`. The 3D renderer
keeps each texture in a layer of an array of textures its size. deko3d lays
out an image 8 to 64 texels tall with a tile taller than the image; the GPU's
texture unit shrinks that tile to fit, but deko3d's copy into the image does
not, so in a texture at least 32 texels wide everything past its first 16
texels across was written into the next layer, while the GPU drew from
memory nothing had written. The patch gives those arrays a tile that fits.
melonDS's own Switch app has the same fault: built from this checkout
(`make -C melonds_deko standalone`), it draws the same patches in the same
battle menu. It was never anything Dualscreen does around the emulator.

`renderer=soft` ("Graphics drawn by" in Settings > melonDS) swaps the deko3d
renderers for melonDS's software ones, which draw on the CPU -- slower,
especially in 3D, but a reference for what the GPU should draw: the patches
went with it, which put them in the deko3d renderers. `dump=N` writes the
frames N to N+7 after the game starts and after each save state is loaded to
`/switch/dualscreen/dump/`, raw (256x384, XRGB8888, the upper screen first).
Played on from a save state with no input, a game draws the same frames every
time, so the same frames from the two renderers can be compared pixel for
pixel: with the fix, Bowser's Inside Story's battle and overworld come out
identical from both, where before 16x8 blocks differed.

The copy that takes each finished frame off the GPU used to read some of it
from memory the GPU had not yet written back from its L2 cache; it now
bypasses that cache, and the fence the CPU waits on flushes it. That was a
real mistake, but not the patches: they were still there after it.

vramcheck's reports of sprite memory "uploaded and not on the GPU" came from
the same mistake in its own readback: what it read was partly the check
before's. `gpucache`, `l2flush` and `settle` were tried against those, and
are left in for now.

`vramcheck=1` looks for memory written that the emulator never wrote. Between
frames, with the GPU idle, it compares the DS's video memory, melonDS's copy
of it, and the 2D renderer's copy of that on the GPU; apart from what the
emulator has marked as changed and not yet uploaded, they must be identical.
Each difference goes in the game log as a `vramcheck:` line saying which copy
of what disagrees, where, and the bytes expected and found, and every 20
checks a line counts them. For a copy on the GPU found stale after an upload
with the right bytes, it also says which upload the GPU ran last for that
block, and whether the staging memory the copy read from still holds the
right bytes.

`pipeline=0` and `gpu_alone=1` together keep the emulator's GPU work and the
app's from ever running at the same time.

`texcache=0` and `shaders=standalone` were for finding out where wrong 3D
textures came from, before the cause above was found; they stay for
comparisons. `texcache=0` is slow -- every texture is decoded and
uploaded again each frame -- but if the textures come out right with it, the
cache is what goes wrong. `shaders=standalone` loads the renderer's GPU
programs from melonDS's own Switch release, found at `/switch/melonDS.nro`
(or `/switch/melonds/melonDS.nro`; `shaders=<path to its .nro>` names another
place), instead of the ones this build compiles with today's shader compiler;
if the textures come out right with it, the shaders are what differ. Try
them one at a time.

The core is compiled with `-fno-strict-aliasing`: melonDS reads its video
memory's byte arrays through wider pointers, which a newer GCC at `-O3` may
compile into reads of stale values. A core built before this is rebuilt
automatically the next time `make` runs.

Two builds are for comparing with melonDS's own app and with the compiler:

- `make -C melonds_deko standalone` builds melonDS's own Switch app from the
  same checkout, with today's devkitPro and without the fixes in `patches/`,
  as `cores/melonDS-today.nro`. It draws the patches above just as Dualscreen
  did before the fix.
- `make OPTIMISE=-O1` builds the core, and Dualscreen with it, with fewer
  compiler optimisations. It made no difference to the patches. A plain
  `make` afterwards puts `-O3` back.

Every line is optional. The game log says what is in use when a game starts.

Cartridge saves go where libretro's melonDS puts them,
`/switch/dualscreen/saves/<game>.srm`, so a game saved with one plays on with
the other. melonDS's own Switch app keeps its saves beside the ROM instead;
copy one across to carry it over.

#### libretro's melonDS, drawing in software

```sh
git clone https://github.com/libretro/melonDS cores/melonDS
make -C cores/melonDS platform=libnx JIT_ARCH=aarch64 ASFLAGS=-D_POSIX_C_SOURCE=200809L \
    AR=$DEVKITPRO/devkitA64/bin/aarch64-none-elf-ar -j8 &&
  cp cores/melonDS/melonds_libretro_libnx.a cores/ && make
```

`JIT_ARCH=aarch64` turns on melonDS's **JIT**, which translates DS code into
the Switch's own rather than interpreting it one instruction at a time.
libretro's build leaves it off for the Switch, with a note about memory
problems still to be solved, but melonDS's own Switch port (RSDuck's) always
ran with it, and here it is the difference between a game running at about
60% of full speed and at full speed: Mario & Luigi: Bowser's Inside Story
went from 27 ms a frame to 13, where a frame has 16.7. `make` says whether the
core it found has it. If a game crashes with it, the game log says where
(a line starting `crashed:`), and building the core again without `JIT_ARCH`
-- after `make -C cores/melonDS platform=libnx clean`, since every file
compiles differently with it -- gives the slower core back.

`ASFLAGS=` because one file the JIT brings in, borrowed from Dolphin, calls
the POSIX `strerror_r`, which devkitPro's C library only declares when POSIX
is asked for -- and melonDS compiles as strict C++17, which does not ask. With
current devkitPro the build stops at `src/dolphin/CommonFuncs.cpp` without it.
melonDS's Switch flags happen to start from `ASFLAGS`, which nothing else
sets, so this reaches every C++ file without replacing any of the others.

`AR=` with the whole path because of how its last step runs. devkitPro's
rules put the toolchain on the path for the commands `make` hands to a shell,
but a command with nothing in it for a shell to interpret -- the archiver line
-- is started directly, and the `make` that comes with macOS looks for it on the
path it was itself started with, which does not have devkitPro's tools on it.
Every compile succeeds and then `aarch64-none-elf-gcc-ar: No such file or
directory` at the end. Naming the archiver in full works everywhere, and plain
`ar` makes the same archive.

The `&&`s stop an archive that failed to build from being copied over one that
worked.

`make` looks for `cores/melonds_deko_libnx.a`, then
`cores/melonds_libretro_libnx.a`, and says which it found. A core anywhere
else, or named differently, is given as `make CORE_LIB=/path/to/core.a`. With
no core, the build is the companion alone, exactly as before.

**Speed.** While a game plays here the app also runs the CPU at 1785 MHz
rather than the 1020 MHz programs get by default -- the speed the Switch's own
games use while loading -- and puts the threads melonDS, the sound and the
network start on cores of their own, where otherwise every one of them would
share the main thread's. It puts the clock back when the game ends. melonDS's
Switch port did both. The game log says how fast a game is running every ten
seconds, in lines starting `speed:`.

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
