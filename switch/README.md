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
| Choose which screen to be | the opening screen, with the d-pad or left stick |
| Move around the menus | d-pad or left stick |
| Join, and confirm in menus | the east face button (A) |
| Go back, or leave a menu | the south face button (B) |
| Open this device's menu | **+ and -** together, or a **two-finger tap** |
| Move around the menus | d-pad or left stick; the east face button confirms, the south one goes back |
| Smooth or sharpen the screens | **R3** — the host applies it on both devices |
| Swap the two focus layouts | **L3** — likewise |
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
step, which is why this directory holds a single file — the loop around them
and the console's own start-up.

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
