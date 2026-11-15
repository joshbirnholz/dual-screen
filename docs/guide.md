# The Dualscreen guide

Everything Dualscreen does, in detail. The [README](../README.md) is the short
version; [technical.md](technical.md) is how it works underneath.

- [What you need to supply](#what-you-need-to-supply)
- [Starting a game](#starting-a-game), [Steam Input](#steam-input)
- [The quick menu](#the-quick-menu)
- [Layouts](#layouts)
- [Render scale](#render-scale), [Screen Smoothing](#screen-smoothing),
  [The right stick](#the-right-stick), [Rumble](#rumble), [Run Ahead](#run-ahead)
- [Saves](#saves): Save Data Management, save states, Resume from Save State
- [Playing across two devices](#playing-across-two-devices)
- [Wii U games](#wii-u-games)
- [Settings](#settings)

## What you need to supply

**An emulator for each console you want to play.** DS games are played by
melonDS DS and 3DS games by Azahar, both libretro cores that run inside
Dualscreen. Wii U games are played by this project's fork of Cemu, running
beside Dualscreen (see [Wii U games](#wii-u-games)).

On a PC or a Steam Deck, Dualscreen downloads these for you: start a game for
a console with no emulator yet and it offers to download one, or use the
**Download** rows under **Settings > System** (see [Emulators](#emulators)).
A Switch, an Android device and an iPhone or iPad have the DS emulator built
in, and do not play 3DS or Wii U games.

You can use your own copy instead. Dualscreen looks in the usual RetroArch
core folders and `/usr/lib/libretro`. For 3DS it prefers Azahar
(`azahar_libretro.so`) and falls back to Citra (`citra_libretro.so`,
`citra_canary_libretro.so`), though only Azahar reads the gyro. Point it at a
particular one with the `core_path` and `core_path_3ds` settings in
`settings.json`, `wii_u_program` for Cemu, or `--core` for a single run.

You only need the emulator for the console you actually play; a missing one is
reported when you try to load a game for it, not before.

3DS games need **OpenGL 3.3**, which any machine able to run a 3DS emulator
has. Without it you get an error screen saying so. DS games need much less.

**BIOS/firmware, if you want them.** melonDS DS boots DS games using its own
open-source BIOS replacement, so nothing is strictly required for DS. For
native boot or DSi mode, put your own dumps (`bios7.bin`, `bios9.bin`,
`firmware.bin`, and the `dsi_*` files) in the system folder. 3DS games need the
system files Azahar asks for, which come off your own console. **No
Nintendo-owned data ships with this app and none is downloaded** — these have
to be your own. The Settings screen shows exactly which DS files it can and
cannot see.

**ROMs.** Yours, obviously.

## Starting a game

```sh
dualscreen --game ~/roms/nds/game.nds
```

That boots straight into the game with its last-used layout — no launcher in
the way.

Run it with no arguments and you get a file browser instead, which opens in the
folder you were last browsing (and, on a first run, wherever your ROMs most
likely are — the EmuDeck and RetroDECK paths, the SD card, then home). Useful
for one catch-all shortcut; per-game shortcuts with `--game` stay the fastest
way in.

Names in Japanese, Chinese or Korean — a ROM's file name, a game's title in
Save Data Management — are drawn with the system's own font for them, since the
font the app carries has only Latin letters: whatever fontconfig picks for
Japanese on Linux (most desktops have Noto Sans CJK; where there is none,
install `fonts-noto-cjk` or similar), Hiragino on a Mac, and the console's own
fonts on a Switch. The log says which it found. With none, those characters
are left out.

`dualscreen --help` lists every option; the useful ones are in
[technical.md](technical.md#the-command-line).

### In-game controls

| | |
| --- | --- |
| Open/close the quick menu | **L3 + R3**, **F1**, a **two-finger tap**, or a button mapped to **Open Menu** in **Controls**. On Android, iPhone and iPad, a controller's Home button too |
| Smooth or sharpen the screens | **Screen Smoothing** in **Game Options**, **F2**, or a button mapped to **Screen Smoothing** |
| Swap the screens | **L3** on a 3DS game (on a DS game L3 is the microphone, below), **F3**, or a button mapped to **Swap Screens**. In the two focus layouts it swaps between them; in Separate Screens it swaps which screen each device shows |
| Blow into the microphone | **L3**, held, on a DS game, or a button mapped to **Blow into Mic** |
| | *DS and 3DS only. A Wii U game reads both sticks pressed and plays through its touch screen, so none of the shortcuts above are taken from it: not L3, not R3, not the pair, and not the two-finger tap. There the menu is **F1**, **Tab** or **Escape**, or a button mapped to **Open Menu**, and **F3** swaps which screen a lone device is showing.* |
| Open Settings, from inside that menu | **+** (or **F4**), on every console — it is a menu shortcut rather than one taken from the game. The settings are whichever device is running the game, whoever asked; with two connected, the other one says to look over here and its buttons still work the screen |
| Move with the circle pad | the left stick, on 3DS games — where it is the circle pad and only that |
| Move with the C-Stick | the right stick, in the handful of 3DS games built for a Circle Pad Pro or New 3DS |
| Touch with a pad | the right stick moves a pointer over the touch screen and **R3** touches with it — on DS games by default, and on 3DS games with **Right Stick** set to **Touch Cursor** |
| Move with the d-pad | the d-pad; on DS games, which have no stick, the left stick works too |
| Touch the touchscreen | tap or drag on whichever screen is showing the bottom screen — a tap that misses the screen itself lands on whichever edge it's nearest to |
| Point with a mouse or trackpad | the pointer appears when you move it and hides again after two still seconds |
| Hide the pointer in games | **Hide Cursor**, under **Game Options** in the quick menu — on a Wii U too, for Cemu's window |
| Move around the menus | d-pad, left stick or arrow keys, held down to keep moving; **A** confirms and **B** goes back, unless **Confirm/Cancel** swaps them |
| Page through a long list | **L** and **R**, or drag the list with a finger |
| Press a button prompt without the button | click or tap the prompt at the bottom of a page. Held down on it, it lights up behind itself, for exactly as long as letting go would press it -- slide off and it goes out, and letting go does nothing |
| Change how button prompts are drawn | **Controller Icons** in Settings: a Switch's, a PlayStation's or an Xbox pad's buttons -- face buttons, shoulders, triggers, start and select, sticks and d-pad, each as that pad prints them, from Kenney's Input Prompts -- or **Generic**: four dots with the one to press filled in, for a pad whose letters do not match the prompts' |
| Swap which button confirms and which goes back | **Confirm/Cancel** in Settings, shown as the two buttons themselves: (A)/(B) or (B)/(A) in the symbols chosen above. Neither setting is on a Switch, which always draws its own letters and confirms with A -- or, with one Joy-Con held sideways, draws the buttons by position, as the Switch's own menus do |

### Adding a Steam shortcut

Add `dualscreen` as a non-Steam game, then set the launch options to
`--game "/path/to/rom.nds"` — one shortcut per game. Each one boots into its
own remembered layout, which is the whole point.

## Steam Input

Launched from Steam, your pad reaches the app through Steam Input, which
matters in three ways.

**Give the menu a button of its own, if you like.** Steam claims the Guide
button, so on a Steam Deck the quick menu opens with **L3 + R3** or a button
mapped to **Open Menu** in the game's **Controls**. Or add a keyboard binding
for **F1** to a spare input in the shortcut's controller layout (a back
paddle, a trackpad click) and that becomes your menu button. A Wii U game
reads both stick presses, so there it is **F1**, **Tab** or **Escape**, or
**Open Menu** in its button mapping.

**Use a gamepad layout, not a desktop one.** In the shortcut's controller
settings, pick a template that sends real gamepad buttons. A keyboard-and-mouse
template sends keystrokes instead, and while the menus do accept Enter, Space,
Escape and Backspace so such a layout is still usable, the game itself will not
see DS buttons at all.

**If a button does the wrong thing**, run once from a terminal with `--verbose`
and press it. Every controller button and key is logged with the name SDL gives
it, along with the pad's name and the type SDL thinks it is:

```
[dualscreen][info ] controller connected: Steam Virtual Gamepad (SDL type: Xbox 360, face buttons read as labels)
[dualscreen][debug] controller button down: a (0)
```

That says exactly what arrived, which is the fastest way to tell a Steam Input
remap from an app bug.

### Face buttons

**Steam Input is the one word on what a button is.** A pad Steam presents is
read by the letters Steam assigns its buttons, whatever the controller
underneath: Steam's A is A, B is B, X is X and Y is Y, in games and in menus.
Remap a button to A in Steam and it is A here. While Steam presents
controllers, the raw copy SDL may also show of one is never read. **Controller
Icons** only changes the pictures (on such a pad an Xbox prompt for A draws
Xbox's A), and **Confirm/Cancel** only which of A and B says yes.

A pad SDL reads itself -- without Steam Input -- uses **Nintendo positional**
controls:

```
        north = X
   west = Y      east = A     confirms menus (or south: see Confirm/Cancel), and is DS A
        south = B             cancels menus, and is DS B
```

So on an Xbox-layout pad read this way, the button marked **B** is DS A and
the button marked **A** is DS B: the places a DS has them. SDL is asked to
report such pads' face buttons by *position* rather than by printed label, and
**Confirm/Cancel** in Settings chooses which of the two lower ones says yes.

Beyond that none of it is configurable. Remap the pad in Steam Input if you
want something else; that is a better place for it than a second set of
bindings here. A pad
SDL has no entry for, or a wrong one, can be corrected without a rebuild by
dropping a `gamecontrollerdb.txt` next to `settings.json`.

The two-finger tap matters: on a tablet with no buttons attached, it is the
only way into the menu. Emulation pauses while the menu is open, and each tile
draws that paused frame in its own arrangement, so you are comparing layouts
against the game rather than against a diagram.

## If it quits immediately in Game Mode

Every run writes a log to:

```
~/.local/share/dualscreen/dualscreen.log
```

It is truncated each run and records the environment, which SDL video driver
was chosen, every OpenGL context that was tried and why each failed, and how
the app exited. In Game Mode there is no terminal, so this file is the only
thing that can tell you what happened — read it from desktop mode after the
failed launch.

Common causes, in the order the log will let you distinguish them:

- **No usable OpenGL context.** The log lists each attempt. The app tries ES
  2.0, ES 3.0, GL 2.1, GL 3.3, and finally whatever the driver offers by
  default, so this means none of them worked.
- **A relative path in the shortcut's launch options.** Game Mode does not
  necessarily start the app in the folder you expect. Use absolute paths for
  `--game` and `--core`.
- **The app is showing an error you never saw.** Errors now stay on screen for
  a moment before any button can dismiss them, so a press delivered as the app
  launches can no longer close one instantly.

## The quick menu

On a DS or 3DS game the quick menu is laid out like Nintendo Switch Online's:
a list down the left, and whatever the highlighted entry holds on the right.

It comes up from the game the way a console's does: the blurred game behind
it fades in quickly, then the menu fades in and grows the last little way
into place, just after -- all in under a quarter of a second, on both
devices. Closed back to the game, it goes the other way: the menu fades and
shrinks first, then the backdrop, the game already going underneath. Going
into one of its pages, or back to it from one, has no animation.

Every list in the app — the main menu, Settings, the file picker, Save Data
Management, this menu's options — stops at its first and last entries rather
than going round. The one exception is this menu's list down the left: a
press past its bottom goes to its top and back, though holding a direction
still stops at the end. Holding a direction anywhere keeps moving, about 13
steps a second after the first.
With a finger, it works as a Switch does: a finger held on a row fills it in,
and lifting it chooses that row, so one tap is enough; sliding off to scroll
lets it go without choosing anything. A list
longer than the screen follows the finger smoothly when dragged, keeps gliding
after a flick and slows to a stop; a touch on a gliding list stops it there
without choosing anything.

| entry | what it shows or does |
| --- | --- |
| **Continue** | back to the game, the same as B |
| **Screen Layout** | the layouts below, each drawn as a picture of the paused game in it, and under them a square button with the Wii U's own stylus-hand symbol: press it to flip the layout in use for the other hand, and the symbol flips to show which. Each layout remembers its own hand. The button is hidden for layouts that are the same either way round, like Vertical Screens. Under Separate Screens the same place has an ↑↓ button instead, which swaps the screens |
| **Create Save State** | the four save state slots (DS only); see [Save states](#save-states) |
| **Load Save State** | the same slots, to go back to one |
| **Save Sync** | this game's page in [Save Data Management](#save-data-management), to sync its save with another device or go back to an earlier one (DS and 3DS). Leaving the page comes back to this menu |
| **Game Options** | the settings you might change mid-game, kept for each game: **Render Scale** (3DS; see [Render scale](#render-scale)), **Sound** (with two devices), **Hide Cursor**, **Rumble** (DS; see [Rumble](#rumble)), **Screen Gap** (one choice for every layout that has a gap: Vertical Screens and the two focus layouts), **Screen Smoothing**, **LCD Filter** (DS: the screens drawn with a handheld LCD's pixel grid over them, as Nintendo's own classics apps offer), **Load Custom Textures** (3DS, with Azahar: texture packs from `saves/Azahar/load/textures/<title id>/`) and **Resume from Save State** (DS; see [Resume from save state](#resume-from-save-state)) |
| **Controls** | **Change Controls**: what each of the pad's buttons does in this game, with turbo and profiles (see [Change Controls](#change-controls)), and the **Right Stick** row (see [The right stick](#the-right-stick)). A Wii U game's Controls are its GamePad and players instead (see [Players](#players)) |
| **Disconnect** | ends a two-device session; only there while one is running |
| **Reset Game** | restarts the game, after asking |
| **Quit** | closes the game, after asking. Chosen on the companion, it closes the app there too rather than going back to its main menu (except on an iPhone, iPad or Apple TV, where apps do not close themselves) |

The menu opens on Continue the first time, and after that on whichever entry
it was last closed on. Up and down move through the list, and the right-hand
side follows. Right or A goes across onto it, always onto its first thing, top
left; left from its edge comes back. B resumes the game from anywhere in the
menu, the list or the right-hand side. **+** opens Settings. A Wii U game's menu has the same
shape, with its own entries (see [Wii U games](#wii-u-games)).

### Your DS's name, favorite color and birthday

**Nickname**, **Favorite Color**, **Birth Month** and **Birth Day**, in
Settings, are what a DS's own settings keep about its owner: a DS game that greets you by name
uses the nickname, one that themes itself uses the color, and one that marks
your birthday uses that. melonDS DS is told all three as a DS game starts.
The name is typed into a field -- on a Steam Deck, Steam's on-screen keyboard
comes up for it; on a Switch, the system keyboard -- and is ten characters at
most, as on a DS. The color is picked from a grid of the DS's sixteen, in the
DS's own order, and is Gray until another is chosen; it is the menus' accent
too, exactly as the DS shows it -- the cursor's ring is drawn in it. The birthday is its month and its day, each chosen on
its own, and is January 1 until another is chosen. A 3DS has none of these to set:
Azahar's libretro core takes no name, color or birthday.

### Dropdowns

A setting with several values opens them in a small box beside its row, the
value in effect filled in. The box stays where it opened; a list longer than
it shows scrolls inside it -- with the cursor, a finger dragged up or down, or
the mouse wheel. Every list in the app scrolls with the mouse wheel.

### Touch, mouse and wheel

Every menu works the same under a finger, the mouse and the mouse wheel, on
whichever device it is drawn: a list is dragged, flicked or turned with the
wheel; a tap on a row chooses it; a tap off
an open popup puts it away. A page of the host's drawn on the connected
device -- the quick menu, Graphics Packs, a Wii U player's page, the amiibo
browser -- is one page on both: where it is scrolled, the cursor and any
popup are the same on both screens, whichever is scrolled or tapped, and a
tap there is sent over as the thing it landed on, so the host answers it
exactly as it would its own. (Settings and its pages are each device's own.) Over a Wii U game, the quick menu is
drawn inside Cemu's window, and this project's Cemu build passes the mouse
through to it, wheel and dragging included.


### Sound Effects

Every menu in the app makes a sound when a press does something: the cursor
moving, something chosen, backing out, an error appearing, and the quick menu
opening over a game, whichever device opened it. Closing it is backing out,
and sounds like it, however it was closed. A switch -- an On/Off setting --
has a sound of its own for each way it is flipped, on and off. A direction
that cannot go any further -- up past the top row, down past the bottom, or
sideways with nothing there -- bumps, as a Switch's cursor does: the cursor's
rounded ring jiggles the way it was pushed and makes a small knock. Held down
a list, it bumps once as it reaches the end, rather than on and on while the
direction is held there. Where the cursor is
not a rounded ring, such as the quick menu's sections down its left, it does
not bump. Any other press that changes nothing makes no sound, and nor do a
game's own buttons while it is playing. A popup's Cancel sounds the
same whether it is chosen with A or backed out of with B, and a press that
closes the app -- Quit, or anything that leaves to start it again -- is
silent rather than cut off part way through. With two devices, the quick menu
is heard on both, as it is seen on both, whichever pad is working it: the
device running the game decides each sound, the same way for a press from
either pad, and sends it to the other to play as well -- and so do its
Settings pages opened from the menu, which the other device's buttons work
too.


start-up (`menu sounds ready`, or `no menu sounds:` and why). **Sound Effects**
in Settings turns them on and off (`ui_sounds` in `settings.json`). A companion with
no Settings of its own -- on a desktop, an iPhone, an iPad or an Apple TV --
always plays them.

## Layouts

These are the Wii U Virtual Console's own Screen Settings, in its order and
under its names — that list is what this app is trying to be.

A game starts in **Vertical Screens** the first time it is played, and after
that in whatever it was last played in.

In the menu's order:

| id | name | what it shows |
| --- | --- | --- |
| `vertical` | Vertical Screens | the DS's arrangement without the console around it |
| `upper_focus` | Upper Screen Focus | the upper screen large, the touch screen small in the corner |
| `touch_focus` | Touch Screen Focus | the touch screen large, the upper screen small in the corner |
| `horizontal` | Horizontal Screens | both screens upright at the same size, next to each other |
| `side_by_side` | Side-by-Side Screens | held sideways: both screens turned a quarter turn |
| `upper_on_tv` | Separate Screens | one screen per device, each filling its own display — and alone, one screen and nothing else |

**Separate Screens** with nobody else connected shows just one screen, filling
the display: the one to watch in a game where only one of the two matters. The
↑↓ button beside the layouts swaps which, and so does **Swap Screens** while
playing (**L3** on a 3DS game, **F3** on a keyboard, or any button mapped to
it in **Change Controls**); each game remembers which screen it was last shown
on. With a second device connected it is one screen on each, and the same
swap trades them between the two, from either device.

The two focus layouts are the same pair of screens with the emphasis reversed,
and which one you want changes with what the game is doing, so **Swap
Screens** swaps between them while playing, with no menu and no pause,
keeping whichever hand the stylus is set for. On a DS game **L3** is the
microphone instead: held, it blows into it, for games that ask to be blown
into. Either can be put on any button in **Change Controls**. Like everything
on that menu it is one choice for both devices: pressing it on the companion
asks the host, the same as picking the layout from the menu would.

The hand button under the layouts mirrors the layouts that put one screen
beside another, so the touch screen is under whichever hand holds the stylus.
It is one toggle rather than a left-handed twin of every layout, and it turns
the sideways screens the other way too.

`vertical`, `horizontal` and the two focus layouts have **Screen Gap** in
**Game Options**, on by default, for the space between their screens. Side-by-Side is the one
exception — its gap stands in for the DS's own hinge, so it stays fixed —
but everywhere else, screens flush against each other are a real option, not
a broken one, so the gap is left up to the player. The layout previews update
live as the toggle changes, the same as picking a different layout does.


Every layout works on both consoles. A 3DS's two screens are different shapes —
400x240 above, 320x240 below — and each layout measures each screen against its
own size rather than assuming they match, so the wider upper screen stays wider
and the narrower touch screen stays centred beneath it — `standard` included,
which has its own 3DS to draw them into.

`standard`, both screens inside a drawing of the console itself, is still
there for `--layout standard`, but it is hidden from the menu, and a game that
remembered it starts on Vertical Screens instead.

`upper_on_tv` is the two-device layout: each device fills itself with the screen
it owns. Pick any other and both devices show the whole thing, which is what you
want when someone is watching over your shoulder. With no second device
connected it falls back to showing both screens, since half a game is not
useful.

## Render scale

A 3DS renders at 400x240 and 320x240. Azahar can draw the same frame at a
multiple of that, and **Render Scale** in the quick menu's Game Options chooses
one while the game runs: **Auto**, or any scale the core offers -- **1x** up to
Azahar's **18x** (an older Citra stops at 10x). It changes how much the
emulator draws and nothing else: the screens stay exactly where the layout puts
them, at the same size, and only get sharper. A game remembers what it was last
set to, and starts at 1x until it is set; the value a new game starts from is
`"default_render_scale"` in `settings.json` (0 for Auto).

**Auto** chooses the least scale at which no screen is drawn bigger than the
emulator draws it -- on this device's screen, and on a connected device's,
whichever screen each shows -- and chooses again as the window, the layout or
the devices change. A screen a tenth or less over a whole scale stays at it.
On a Deck's 1280x800 in Vertical Screens that is 2x, the upper screen 800x480
for about 533x320 drawn; on a 4K television with the upper screen filling its
height, 9x. A higher scale than that draws pixels nothing can show, and a
handheld GPU pays for every one.

It is shared like everything else on that menu. Only the host is running a core,
but the menu is one menu reachable from either device: a companion shows the same
button, and pressing it asks the host, which changes the core and broadcasts the
result back. Neither device decides it locally, so the two cannot disagree.

A DS game does not have the button at all: melonDS is pinned to its software
renderer, which draws one pixel per DS pixel and has nothing to multiply.

## Screen Smoothing

A DS screen is 256x192 and a Deck's is 1280x800, so every pixel is drawn several
pixels wide. **Screen Smoothing** in **Game Options** chooses how: off (the
default) keeps the original pixels square and sharp, on blends between them.
**F2**, or a button mapped to **Screen Smoothing**, toggles it from a game, so
the difference can be judged against what is actually on screen. A game remembers which it was last given; the setting a game starts
from is `"default_bilinear_filtering"` in `settings.json`.

It applies to streamed screens too, and while two devices are connected it is one
choice for both of them, like everything else on that menu. A picture that
had to be shrunk or stretched by an amount that is not a whole number, such
as a stream fitted to a smaller display, is always smoothed, and a Wii U game
always is.

## Change Controls

**Controls > Change Controls** in a DS or 3DS game's quick menu lists each of
the pad's buttons and what it presses, with the console drawn beside the list
and that button lit. **A** picks another button for one: one of the
console's, or one of Dualscreen's own (**Open Menu**, **Swap Screens**,
**Next Screen Layout**, **Screen Smoothing**, **Blow into Mic** on a DS,
**Touch Point**, **Activate Touch Cursor**), or **Disabled**. **Y** puts a
button on turbo, and **X** puts every button back. The mapping is kept for
each game, and it is the host's, so both devices' pads follow it.

**Touch Point** touches the touch screen at a spot you choose for as long as
the button is held, for a game that wants the same spot touched over and over.

**Save Profile…** and **Load Profile…**, at the top right (**Right** from the
list), keep a mapping under a name for any game to load. There is a list of
profiles for DS games, one for 3DS games, and one for each Wii U controller
type.

## The right stick

A 3DS has a circle pad of its own, and the handful of games built for a Circle
Pad Pro or a New 3DS read a second stick as well. Nothing in a ROM says which,
so the **Right Stick** row at the foot of **Controls > Change Controls**
chooses between:

- **C-Stick** — a Circle Pad Pro / New 3DS second stick. The default on a 3DS.
- **Circle Pad** — a second way to drive the circle pad, alongside the left
  stick. The stronger of the two wins, so pushing either one walks.
- **D-Pad** — pressed directions, the way the left stick works on a DS.
- **Touch Cursor** — a stylus: see below.
- **Touch Analog Stick** — a stick drawn on the touch screen: pushing the right
  stick touches out from its centre as far as it is pushed, and letting go
  lifts, for a game steered by dragging. Choosing it opens a page to place it
  on the paused game's touch screen (moved with either stick, the d-pad or a
  finger) and size it with **L** and **R**.
- **Off** — ignored entirely, for a game that does something unwanted with a
  stick it is handed.

Each mapping reaches exactly one destination. A stick feeding two at once is
what made the circle pad unusable back when the left stick was also the d-pad:
a core that reads the d-pad sees a direction held down for as long as the stick
is off centre.

A DS has no stick at all — its left stick already stands in for the d-pad — so
its right stick is **Touch Cursor** (the default there), **Touch Analog Stick**
or **Off**. A Wii U's
GamePad has two sticks of its own and is not asked.

A game remembers what it was last set to; the value a new game starts from is
`"default_right_stick_mode"` in `settings.json` (a DS game given one it does
not have starts on Touch Cursor). Like everything else on that menu it is one
choice for both devices, since both devices' pads drive the same game.

### Touch Cursor

A pointer over the touch screen, for touching without a finger or a mouse — a
pad on a television, or a device that is not the one showing the touch screen.
The right stick moves it: a little push creeps, for picking out one small
button, and a full one crosses the screen in under a second. **R3** is its tip:
held, it touches where the pointer is, so holding it and moving the stick
drags. A finger on the screen still wins while it is there.

The pointer shows while it is moved or pressed and fades a couple of seconds
after, and is drawn wherever the touch screen is — on both devices when one is
connected, whichever pad moved it. **L3 + R3** still opens the menu without
touching on the way.

## Rumble

A DS game rumbles through a Rumble Pak in the GBA slot, and **Rumble** under
Options in the quick menu puts one in or takes it out, per game. Off by
default: nothing in a DS game says whether it knows what a Rumble Pak is, so it
is offered for every DS game, and a game that does not use one gains nothing.
While it is in, the game's rumble reaches every pad on both devices. A game
that looks for one only as it starts sees it after **Reset Game**.

It is only there when the core can put a Rumble Pak in at all -- found, like
the resolution, by what the core declares: a slot device one of whose values is
a Rumble Pak. The log says which it found.

## Run Ahead

A game has input lag of its own: it reads the touch in one frame and draws the
result a frame or two later, on a real DS as much as here. **Run Ahead** in
Settings > DS & 3DS (off, 1 frame or 2 frames) hides that, the way
RetroArch's run-ahead does. Each frame the game is run normally and its sound
played. It is then saved, run on silently by the chosen number of frames, and
the last of those is the picture shown. Then it is put back. What is on
screen is where the game will be a frame or two from now, which is exactly the
delay the game itself would otherwise add. One frame of it takes back about
what the network costs.

It costs a whole frame of emulation per frame hidden, plus a save and a load
each frame, so it is off by default. After a few seconds the host logs
`run-ahead is costing Nms a frame…`. If that approaches the 16.7ms a frame
has, turn it down. It needs a core that can save states, which melonDS does.
One that fails to save or load a state turns run-ahead off and says so in the
log. Set too high for the game's actual lag, it shows frames the game would
not really reach, which reads as a flicker when the input changes. One frame
is the safe setting, and it applies the moment it is changed.

## Saves

### Cartridge saves

Cartridge saves are written to `~/.local/share/dualscreen/saves/<rom name>.srm`
(`%LOCALAPPDATA%\dualscreen\saves` on Windows; see
[where things are kept](technical.md#where-things-are-kept)), the same format and naming RetroArch uses — copy an existing `.srm` in and it
will be picked up, or copy one out and RetroArch will read it.

They are written whenever the game changes them, checked every few seconds, and
also on losing focus, on opening the menu and on quitting. A handheld gets put
to sleep and powered off without warning, so waiting for a clean exit is not
good enough. Each write goes to a temporary file that is flushed to disk and
then renamed over the old one, so an interrupted write cannot leave a truncated
save behind.

DSi NAND and firmware saves are the core's own business and live in the system
folder.

### Save Data Management

**Save Data Management**, in Settings > System,
lists every DS, 3DS and Wii U save on this device with the game's own icon,
title and publisher, newest first, each marked with its console. **X Sync All** brings every game up
to date with every other device found: each game's newest save goes to the
device without it, including games only the other device has, and a game
both have changed since they last matched is asked about, one at a time.

Choosing one opens its page, laid out like the Switch's save data cloud page.
During a game, **Save Sync** on the quick menu opens that game's page directly,
and leaving it goes back to the menu:

- **Sync with _device_** gets or sends the save, whichever leaves the newest
  on both, without asking — unless both have changed since they last matched,
  when it asks which save to keep.
- **Get Save from _device_** and **Send Save to _device_** move the save
  between this device and another one on the network. Each asks first,
  showing both saves and their dates, and says when the save being replaced
  has progress the other doesn't.
- **Earlier Saves** holds the last 10 saves replaced or deleted here, each
  with where it was made and what replaced it. Any of them can be restored.
- **Delete Save Data** moves the save to Earlier Saves.

On the right are the other device's save above and this device's below, with
a box saying how they compare: *Up to date*, *Newer on …*, *Only on …*,
*Currently playing on …* (nothing is offered for a game being played there),
or *Save file conflict* when both have changed since they last matched.
Which save is newer comes from each save's history of versions, not from the
two devices' clocks. The page looks for devices each time it opens;
**L**/**R** changes device when more than one is found, and **Y** searches
again. With nothing found, only this device's save is shown. A save another
device sends here shows up at once on whatever this device has on screen.

Nothing happens on its own: saves only move when you choose. Every device
that plays games answers (a Steam Deck, a PC, a Switch, an Android device or
an iPhone or iPad playing DS games) and
any Dualscreen on the network is trusted, with no pairing. Devices that only
show another's screens have no saves and are never offered. Devices are found
on UDP port 47823 and saves move on TCP port 47824. Saves are matched
by the game itself (the code, version and checksum in the ROM's header), so a
renamed ROM still finds its save, and different regions stay separate.

A resume point holds the save as it was, so replacing, restoring or deleting a
save deletes the game's resume point too — and asks first.

A Switch keeps its saves as `<game>.srm` too now, like melonDS DS; a
`<game>.sav` from before is renamed the first time the game starts.

### 3DS and Wii U saves

A 3DS or Wii U game's save is a folder rather than a file, kept where the
emulator keeps it, and Save Data Management does everything with it that it does
with a DS game's `.srm`: versions, Earlier Saves, Sync, Get, Send, Delete and
Sync All. It is known by the game's title ID, so the same game matches on
every device whatever its file is called.

- **3DS**: Azahar (or Citra) keeps a game's save in
  `saves/Azahar/sdmc/Nintendo 3DS/<zeros>/<zeros>/title/<high>/<low>/data`
  under this app's saves folder, since that is the folder the core is given.
  The title, publisher and icon come from the ROM (`.3ds`, `.cci`, `.cxi` or
  `.cia`) when it is decrypted, as Azahar needs it to be; the save is matched
  to its ROM in the folders the file picker looks in. Extra data (`extdata`),
  which a few games keep beside their save, is not included.
- **Wii U**: Cemu keeps a game's save in `mlc01/usr/save/<high>/<low>`. The
  `mlc01` folder is found from Cemu's own `settings.xml` — a portable Cemu
  beside `wii_u_program`, then `~/.config/Cemu`, the Flatpak's and a Mac's —
  or its default place beside it; set `"wii_u_mlc_path"` in `settings.json`
  to name it outright. Without it, no Wii U saves are listed and none are
  accepted from other devices. The game's title, publisher and icon come from
  its `meta` folder, wherever the game is: installed in `mlc01/usr/title`
  (the game's, or its update's); else wherever Cemu's game list says it is,
  from the list Cemu keeps of it (`title_list_cache.xml`, in its user-data
  folder) — a folder with `code`, `content` and `meta` in it, or a `.wua`,
  read from inside (that needs this app built with zstd, which `cmake` finds
  on its own and says whether it did). A `.wud` or `.wux` is encrypted, so for
  one of those Save Data Management shows the name Cemu's list kept, and no icon.
  A game listed before its name was found is looked up again the next time
  the app starts. A game started from here is known by its folder's
  `code/*.rpx`; one started from a `.wud`, `.wux` or `.wua` is not identified
  at launch, but its save is still listed from Cemu's folder.

A save changed in the emulator on its own is seen as a new version the next
time Save Data Management lists it. A Switch keeps DS saves only, so it is never
offered a 3DS or Wii U save, and Sync All leaves those games out between it
and another device. Saves only move between devices running this version or
later; an older one is only ever asked about DS saves.

### Save states

The quick menu of a DS game has **Create Save State** and **Load Save
State**. Either shows four slots on the right, two by two, like Nintendo
Switch Online's suspend points: each an outlined box with a picture of both
screens, one above the other, and its name ("Save slot 1") with the date and
time it was made, or "Empty". Going across puts the cursor on the first slot.

Dates and times are written the way the device showing them writes them:
`09/24/2026 - 10:38 PM` in the US, `24.09.2026 - 22:38` in Germany,
`2026/09/24 - 22:38` in Japan. On a Mac, iPhone, iPad or Apple TV that follows
the region and the 24-Hour Time switch in its settings; on Linux and SteamOS,
the system locale; on a Switch, its system language. A companion shows them its
own way, whatever the host does.

- **Create** into an empty slot saves straight away. Saving over a slot that
  already has something in it asks *Overwrite this save state?* first. A save
  made is heard as a short chime, on both devices, and seen as the slot's new
  picture; only a save that fails says so in a popup.
- **Load** asks *Load this save state?* and then goes straight back into the
  game, from that moment. Empty slots are dimmed and do nothing.

A save state is the emulator's own snapshot of the whole console at that
moment, and is separate from the cartridge save above, which carries on being
written as usual. So loading one also puts the cartridge save back to how it
was then, the same as it would on the console. They are kept beside the
cartridge save as `<rom name>.savestate1` to `.savestate4`, written the same
all-or-nothing way (any from before the rename, `.savepoint1` and so on, are
renamed the next time the menu opens). They are this app's own format, not
RetroArch's. A save state made with one version of a core may not load
in a much newer one; the menu says so if it does not.

The entries only appear for a DS game, and only when its core can make save
states (melonDS DS can). A 3DS game has none: making one froze the app on a
Steam Deck, so a 3DS game saves the way it does on the console, from its own
menus. The saves are made and kept by the
device running the game; a companion's copy of the menu is sent the slots,
pictures and all, and saves and loads through the host like any other press.

### Resume from save state

**Resume from Save State**, under **Game Options** in a DS game's quick menu, is
per game and off until it is turned on. Off, the game starts from the
beginning as always. On, it starts from wherever it was when the app last
closed -- paused, under a popup that says *Resuming from save state.* with an
**OK** to put it away and carry on. The popup is on both devices when a
companion is connected, and OK on either one starts the game. Like
Switch Online's suspend point, it does not matter how the app was left:

- **Quitting**, from the menu or with SteamOS's *Exit game*, writes it on the
  way out. So does starting another game from the browser.
- **The quick menu opening**, and **the window losing the focus** -- the Steam
  button, or the Deck going to sleep -- each write one, since either is often
  the last thing before a game is put down.
- **Every in-game save** writes one at the same moment. Loading a state puts
  the cartridge save back to how it was when the state was made, so a resume
  point older than the last in-game save could undo that save; kept in step,
  it never can.
- **Every 30 seconds of play** otherwise, for what gives no warning at all: a
  flat battery or a crash lands you at most half a minute back, never behind
  your last in-game save.

It is kept beside the cartridge save as `<rom name>.resume`, separate from the
four slots, and written the same all-or-nothing way, so a power cut while one
is being written leaves the one before it. The game only pauses long enough to
take the snapshot -- about 9 ms for a Mario Kart DS race with the deko3d
melonDS on a Switch, inside the time a frame has to spare, where it was 28 ms
and a dropped frame every half minute before
`switch/melonds_deko/patches/Savestate-memory-stream.patch`; compressing it
and writing it happen on a thread of their own. Turning the option off deletes it, so an old one can never be resumed by
surprise later. If it will not load -- a much newer core, say -- the game
starts from the beginning and the log says why.

## Playing across two devices

Start the game on whichever device you want to run it — a gaming PC, or the
Deck. It announces itself on the network automatically.

On the other device, launch dualscreen with no ROM and pick **Connect as Top
Screen** or **Connect as Bottom Screen**. It finds anything running nearby,
lists it, and joins. The device holding the bottom screen is the one whose
touchscreen works, and both devices' buttons drive the game.

The quick menu opens on either device, in any of the usual ways, and shows
the same thing on both. Everything on it belongs to the host: the companion asks,
the host decides and broadcasts the result, and only then does either device
change. That is what makes the two ends impossible to get out of step, rather
than merely unlikely. Opening the menu on either device pauses the game on both,
so nobody is left playing on alone. The menu carries a **Disconnect** entry on
both ends, so either device can end the session, and a **Reset Game** entry on
both too, since only the host runs a core but either device can ask it to
restart. **Quit** is there on both ends as well: choosing it on the companion
asks the host to quit too, the same as choosing it there, and then leaves the
session the way Disconnect would, back to that device's own main menu.

**Sound** is on the same menu, once two devices are connected, and cycles between
the top screen's device, the bottom screen's device, and both. DS and 3DS have
one soundtrack, so "both" is the two devices playing the same thing — useful when
the Deck is across the room from the TV, and worth turning off on one of them
when they are not.

**+** in the menu opens Settings on the device running the game, whichever
device asked; the other device says to look over there, and its buttons still
work the screen. Each device's own settings are on its home screen.

```sh
# Skip the menus entirely, for a Steam shortcut on the companion device:
dualscreen --connect bottom
```

### Starting a game from the other device

A Linux PC, a Steam Deck or a Windows PC can let another device start a game
on it, with nobody sitting at it and Dualscreen not even open. Choose
**Settings > System > Enable Remote Start**: after an alert saying what it
will do, that installs a small background service (`dualscreen --service`)
that starts at every login: a systemd user unit on Linux, so it runs in Game
Mode too, and a sign-in item on Windows. **Disable Remote Start** removes
the service again, and forgets every device it let in. The service restarts
itself on the new build whenever Dualscreen is updated.

There is no pairing step: a device that joins a game on that computer while
Remote Start is on is let in from then on, and remembers how to reach and wake
it. From then on its home screen -- the Switch, phone or other companion's,
or the desktop app's on a Steam Deck -- shows the games last played on that
computer, newest first, as one row of icons along the bottom. While the
computer is on and found, the row has only the games it can start now; while
it is off or asleep, the row keeps the ones last seen, icons and all.
This is Quick Start, on unless **Settings > System > Quick Start** is
turned off, which leaves the home screen without the row.

Choosing one asks first -- "Start <game> on <computer>?" -- then starts it
there -- through its Steam shortcut, so its Steam Input layout applies, or
from its ROM folders when Steam has none -- and the device joins it as the
bottom screen as soon as it is up, all on one page showing the game's icon
and what is happening, which B leaves. If that game is already running there,
the device just joins it; if another one is, it asks before closing that one
(its save written as it closes) and starting this one in its place. A computer that is asleep
is first woken with Wake-on-LAN, as Moonlight does: a packet for each of its
network cards, sent to the whole network and to its last address, again every
few seconds for up to a minute and a half. If it does not wake, an alert says
so. Wake-on-LAN has to be turned on in the computer's firmware settings and
for its network card, and usually only works over a cable rather than Wi-Fi.
A game counts as played once Dualscreen has started it on that computer.

### The devices that can be the second screen

The other device does not have to be a PC. Any of these can join a game
running somewhere else as one of its two screens, with no emulator, ROM or
BIOS of its own, since everything is being played on the other device:

- the **desktop app** on a Steam Deck, a Linux PC or a Windows PC, with
  **Connect as Top Screen** or **Connect as Bottom Screen**
- a **Nintendo Switch** or **Switch 2**, as homebrew; see
  [switch/README.md](../switch/README.md)
- an **Android** handheld, phone or tablet; see
  [android/README.md](../android/README.md)
- an **iPhone** or **iPad**, an **Apple TV** (as the top screen or a Wii U's
  television, since it has no touch screen) or a **Mac**; see
  [apple/README.md](../apple/README.md)

A Switch, an Android device and an iPhone or iPad can also play DS games
themselves, and host them for another device in turn.

It shares the parts that describe a session — the protocol, discovery, the
layout arithmetic, the controller mapping — with this app rather than
reimplementing them, so a host cannot tell the difference between it and
another PC, and neither can the menu.

Its in-session menu *is* this menu — the same source, the same list, the same
previews, the same options — because the device showing a screen is usually the
one being held, and the alternative is reaching for a pad plugged into the
other machine. The only entry it drops is **Hide Cursor**, which is the one thing
on that menu belonging to the device rather than the host, and a console has no
arrow to show or hide.

## Wii U games

A Wii U is not played the way a DS or a 3DS is. There is no Wii U emulator
that can run inside this app, so a Wii U game is played by **Cemu, running
beside Dualscreen**, which hands its television and GamePad pictures over.
It has to be this project's fork of Cemu
([joshbirnholz/cemu-dualscreen](https://github.com/joshbirnholz/cemu-dualscreen)),
which knows how to talk to Dualscreen. On a PC or a Steam Deck, Dualscreen
downloads it for you and keeps it up to date (see [Emulators](#emulators)).
[cemu-integration.md](cemu-integration.md) is the full account of how the two
work together.

Dualscreen starts Cemu itself when you pick a Wii U game, which is what lets
it work from Game Mode, where there is no desktop to start Cemu from, and
quitting the game stops it again. To use a Cemu of your own, set it in
`settings.json`:

```json
{
  "wii_u_program": "/path/to/cemu",
  "wii_u_arguments": ["--dualscreen-socket", "{socket}", "--game", "{game}"]
}
```

Those arguments are the default, because they are the fork's; only the program
has to be set. `{socket}` becomes the path it should hand its screens over on
and `{game}` the file you picked. Leave `wii_u_program` empty to start Cemu
yourself, which works on a desktop but not from a Steam shortcut. Either way,
a device waiting before Cemu is there says so rather than showing a blank
window.

What is different about a Wii U session:

- **One device shows one screen.** The television is the game and the GamePad is
  a second view of it, so squeezing both into one window is not the arrangement
  the console has. With a second device connected, each holds one. Alone, the
  device is whichever screen **Default Wii U Screen** on the Settings
  screen says — and Cemu is told, so its own window shows that screen rather
  than whatever it was last left on. **F3** swaps it for the rest of a session
  without writing anything down.

  That setting is **Auto** by default. On a Steam Deck's own 1280x800 panel
  the device is the GamePad; on any other display it is the television. So a
  Deck in the hand is the GamePad and the same Deck on a dock is the
  television. It is decided when the game starts. A Deck's display does not
  change size under a running app, so docking takes effect the next time
  Dualscreen is started. **TV** and **GamePad** pin
  it to one. The log says which it picked and why:

  ```
  alone, this device is a Wii U's television (Auto, on a 1920x1080 display, which is not a Steam Deck's own panel)
  ```

  Motion in the GamePad role comes from whatever this device reads its own
  sensors through, which on a Steam Deck in Game Mode means
  [SteamDeckGyroDSU](https://github.com/kmicki/SteamDeckGyroDSU) — Steam Input
  hands SDL a pad with no sensors on it, so there is nothing else to read. See
  **Motion** below. If nothing answers, the log says so:
  `motion: nothing has answered at 127.0.0.1:26760 …`.

  On a desktop, Auto means the television, and the GamePad is then whatever
  you have set up in Cemu. That is how to play the many games that never ask
  for the pad, with a controller already configured for them.

- **The GamePad belongs to one controller, and every other controller is a
  player.** By default the device showing the GamePad screen gives the GamePad
  its first controller. Every other controller on either device is Player 1,
  Player 2 and so on, in order. See **Players** below. A DS or a 3DS is
  different: two people share one console there, so both devices' controls are
  merged.

- **The quick menu is on screen the whole time.** While Cemu's window plays
  the game, this app's own window shows the Wii U menu in place of a
  "Playing on Cemu" notice, laid out as a DS or 3DS game's is -- sections
  down the left, and the highlighted one's things on the right: **Game Options**,
  **Controls**, **amiibo**,
  **Disconnect**, **Reset Game** and **Quit**, with **Settings** on plus as
  on every console's menu. Cemu cannot restart a game itself, so **Reset Game** closes the
  Cemu this app started and starts it again on the same game. It
  answers only while its window has the focus, so a press meant for the game
  never picks Quit. Opened over the game, the menu also starts with
  **Continue**, which goes back to it.

- **The quick menu also opens over the game, in Cemu's window.** With this
  project's Cemu build, **F1** or **Escape** in Cemu's window, or the menu
  button on a pad, puts the quick menu up over the game without switching windows. The game is
  darkened behind it but keeps running, and pads work the menu while it is up.
  It is the same menu on both devices: opened from a companion, it shows in
  Cemu's window too. **F1**, or **B** or **Escape** on the list, puts it
  away. Escape no longer leaves Cemu's fullscreen while this app is attached;
  **F11** or **Alt+Enter** still does. Not **Tab**,
  which Cemu keeps for itself: held, it shows the GamePad's screen.

- **Graphics Packs**, the first of a Wii U game's **Game Options** in the quick
  menu, is Cemu's own Graphic Packs window
  laid out as the quick menu is, so the packs can be changed with a controller
  in Game Mode, where that window is out of reach: the game's packs down the
  left, grouped as Mods, Enhancements and so on, and on the right the one
  highlighted -- on or off, each of its presets, and what it says about
  itself. **Right** or **A** goes into a pack, **Left** or **B** back to the
  list; inside it, **A** turns it on or off, or opens a preset's choices beside it.
  Cemu makes each change as it is chosen and keeps it, exactly as its own
  window would. A pack that replaces textures (most resolution
  packs) is only read as the game starts, and the page then says its change
  will take effect once the game is restarted -- which **Reset**, a step back,
  does. It needs this project's Cemu build. A second device shows the same
  page, the cursor where it is, and works it with its own pad.

- **Game Options**, in the quick menu, are this game's, as a DS or 3DS game's
  are, each stepped on with **A**: first **Graphics Packs**, which opens the
  page above, then **Swap Screens** (played alone: the other
  screen from **Default Wii U Screen** -- **F3** does the same; with a second
  device connected, the two trade screens), **Hide Cursor** (this device's own,
  as on every console's menu; placing a Touch Point always shows it), then Cemu's: **Upscaling** (how Cemu enlarges
  a picture smaller than its window: bilinear, bicubic, Hermite, nearest
  neighbor or **AMD FSR**, with an **FSR Sharpness** slider below it that is
  greyed out unless FSR is chosen), **Aspect Ratio** (the game's own, or stretched to the screen),
  **Performance Overlay** (off, frame rate, frame rate and CPU, or
  everything Cemu can show) and **CPU Mode** (auto, multi-core or
  single-core, for a game that misbehaves). Cemu keeps the first three for
  every game alike, so Dualscreen keeps each game's choice and sets it as the
  game starts, Cemu's default where the game has none; they change at once.
  CPU mode is kept in Cemu's own profile for the game, and changes when the
  game is reset.

- **Controller Type**, on each player's page in Controls, is
  what each player's controller is to the game -- **Wii U Pro Controller**,
  **Wii Remote**, **Wii Remote + Nunchuk** or **Wii Classic Controller** --
  kept for each game and set as it starts, so a game that wants Wii Remotes and one that wants Pro
  Controllers each get theirs without a trip to Cemu's input settings. A
  change happens at once, as if the controller had been swapped. Cemu's own
  saved controller profiles are left as they were: a game with nothing chosen
  gets what Cemu started with.

  A player's controller is only plugged in while a pad is feeding it: unplug
  the pad, or leave it out of Change Order in Controls, and the game sees
  that controller disconnect, as a real one's would; it comes back, as the
  type this game wants, when a pad does. So a game never counts controllers
  nobody is holding.

- **A game's own dialogs** -- its keyboard, when it asks for a name (a horse's
  in Breath of the Wild), and its error messages -- are this app's popups
  rather than Cemu's small windows. The keyboard is the popup a DS nickname is
  typed in, with Steam's on-screen keyboard on a Steam Deck -- both sit in the
  top of the screen, clear of an on-screen keyboard: what is typed in
  Cemu's window shows in it as it is typed, OK hands it to the game, and
  Cancel leaves the game what it started with. A message has the game's own
  buttons, or OK; with only one, B presses it. The game goes on underneath,
  and one put up while the quick menu is open waits until it closes. A
  connected device shows the same popup and can answer it; when it is the
  GamePad -- the device in your hands -- the keyboard comes up there instead
  (a Switch's own keyboard on a Switch), and what is typed shows on both.

- **Wii U TV Audio**, in Settings, is Cemu's TV channels -- **Mono**,
  **Stereo** (Cemu's default) or **Surround** -- the same for every game.
  Dualscreen tells Cemu as it connects, and again when it is changed; Cemu
  opens the TV's sound as a game starts, so a game already running hears the
  change once it is reset. It needs this project's Cemu build.

- **Settings > Wii U** also has Cemu's own: **Pause Wii U Games in the Quick
  Menu**, **Console Language**, **Wii U TV Volume**, **Async Shader Compile**
  (not on a Mac), **Intro Sound**, and under **Notifications** which of
  Cemu's pop-up notices to show. **Download Graphics Packs** fetches Cemu's
  community graphics packs, and later offers to update them. All of them
  need this project's Cemu build.

- **Wii Remote Rumble Strength**, in Settings, is how hard a controller
  rumbles while a game has it as a Wii Remote -- **Off**, **Weak**, **Normal**
  (the default), **Strong** or **Strongest** -- for every game and every Wii Remote.
  A real one's motor is a small one; a pad's at full strength is far louder.
  Not on Android, whose controllers rumble at one strength.

- **amiibo**, in the quick menu, lists the amiibo dumps (`.bin`, `.nfc`
  or `.dat`) in the amiibo folder, and a level or two of folders inside it,
  by name on the right. The folder is `~/amiibo` (or `~/Documents/amiibo`)
  until another is chosen with **Choose a Folder...** at the end of the list,
  which browses to one and scans the amiibo picked there. Choosing one
  closes the menu, and four seconds after the game is going again it is
  held to the GamePad, as Cemu's own "Scan NFC tag/amiibo from file" does --
  a game only sees a figure for the couple of seconds it is there, which a
  paused one would miss, and one coming back from its HOME Menu (Breath of
  the Wild) shows a loading icon of its own for a moment first. Until Cemu
  says it has been read, the top right corner says **Scanning…** -- on the
  connected device too, as every corner alert is; the game says the rest. A file that is not an amiibo dump is said to be so
  in a popup, and the menu stays up.

- **Hide Cursor** hides or shows the mouse pointer over Cemu's window, as it does
  over a DS game. Shown, it appears when a mouse or trackpad moves it -- even
  with Cemu filling the screen, as in Game Mode -- and hides again when it is
  still, and a click with it on the GamePad's screen is a touch there. Hidden,
  it never shows. Touches on the screen itself work either way.

- **None of the controller shortcuts exist.** A Wii U game reads every control
  the console has, including both stick presses, so nothing is taken from the
  pad: no L3, no R3, no chord, no two-finger tap. The menu is always on
  screen on the device running the game (see above), and **F1**, **Tab** or
  **Escape** on a companion, or a button mapped to **Open Menu** in a
  player's **Edit Button Mapping**. Steam Input can be pointed at any of them. Which is
  also why every button on the pad, and both triggers as ZL and ZR, go straight
  through to the game — including from the second device, whose touches arrive
  as touches on the GamePad's own screen.
- **Screens go out as H.264, rather than as changed tiles.** A GamePad screen
  in motion measures at 42.8 Mbit/s as tiles and 2.45 as video; see
  [what streaming a screen costs](technical.md#what-streaming-a-screen-costs). The host picks that by itself; if the other
  device cannot decode video, it sends tiles instead.

Cemu owns everything about the emulation: the resolution it renders at, its own
controller bindings, its own saves. The shell offers no render scale for a Wii U
because that setting is not its to offer.

### Controls, and where they are configured

Usually there is nothing to set up in Cemu: everything is chosen in the quick
menu's **Controls** (see [Players](#players)). A Cemu whose input has never
been configured is given the GamePad on its own, and Dualscreen plugs each
player in as the controller type the game wants.

Underneath, whatever is holding the GamePad screen appears in Cemu's own
**Input Settings** as a controller called **DualScreen GamePad**, under an API
called **DualScreen**, and can be added to Player 1's Wii U GamePad the way
you would any other pad.

One controller, whichever way you are playing: on two devices it carries
whatever the device showing the GamePad screen is holding, and on one it
carries this device's first controller. Set it up once and both work.

That makes it an ordinary Cemu controller in every way that matters. Wii U games
want wildly different control schemes, and this is where they get them: buttons,
both sticks, stick deadzones and motion all go through Cemu's normal mapping and
its per-game profiles.

Rumble goes to whichever controllers are the GamePad, on either device.

Adding it applies a default mapping straight through — A to A, and so on — and
switches its motion and rumble on, so a fresh setup plays correctly with nothing
further configured. Every part of that is yours to change afterwards.

Touch is the exception, and is not in the mapping: the companion's touchscreen
drives the GamePad's directly, because a point on a screen is not a control a
button mapping can name.

### Players

Every other controller reaches Cemu the same way, as **DualScreen Player 1** to
**DualScreen Player 8** under the same **DualScreen** API. Map them in Cemu
once, as Pro Controllers, Classic Controllers or Wii Remotes, and a default
mapping is applied. Which physical controller sits behind each player is
decided here, not in Cemu, so the mapping stays put while the controllers
change.

Controllers on both devices count, and they stand in one line: the first is
the GamePad, the next eight are Player 1 to Player 8, and anything after that
is not used. One controller to each: no two controllers are ever the same
player, and none share the GamePad. By default:

- The device showing the GamePad screen gives the GamePad its first
  controller. A companion with no controller of its own, such as a phone,
  leaves the GamePad to its touch screen and motion.
- Every other controller is the next player: this device's first, in order,
  then the companion's.

Steam keeps spare virtual controllers, and one stays behind after its
controller disconnects. SDL lists those as plain "Steam Virtual Gamepad" with
nothing behind them. They are left out, and a slot reappears as soon as a
controller is behind it again.

Keyboards are not in the line at all. Map a keyboard in Cemu directly, with
Cemu's own **Keyboard** API, if a game wants one.

Controllers start out in each device's own order, which under Steam Input is
Steam's. Each is shown with the name Steam gives it, where there is one, and
which device it is on and which player slot that device has it in, such as
"steamdeck P2" or "Switch Lite P1". Set `device_name` in settings.json to
change what this device is called there. The name is only shown: a virtual pad's name is fixed
when it appears, and after controllers come, go and are reordered it can
describe a different controller from the one behind it.

The quick menu's **Controls** lists the GamePad, then every player this game
has, each with the name of its controller and where it is. A player with no
controller is still listed, dimmed and **Not Connected**: it can be set up,
but the game is not told it is there. **Add Controller**, after the last
player while there are fewer than eight, opens the new player's page to
choose its **Controller Type** first -- the same as the player before it, or
a **Wii U Pro Controller** for Player 1 -- and **Add Controller** there makes
it. A controller arriving takes the first player with none by itself: as
what this game had that player as before -- never **None** -- or, for a
player this game has not had, as the player before it is, mapped the way
Cemu maps a new one. Past the four players a Wii U pairs, it waits to be
taken in Change Order. A player is never a Classic
Controller alone either, which a Wii U never sees -- it is always plugged
into a Wii Remote, whose buttons a game can read too -- so it is a **Wii
Classic Controller**, a Wii Remote with one plugged in: the remote's buttons
can be mapped as well, though few games want them.
**A** (Edit) opens the GamePad's or a player's page: **Controller Type**,
above, **Edit Button Mapping** and, for a player, **Remove Controller**,
which takes the player away and moves every player after it up a place,
types and mappings with them. The GamePad's ends with **Disconnect
GamePad**: the game is played without one, its controller becoming the
first free player -- a single controller on the television as a Pro
Controller. The game remembers it; **Connect GamePad** brings it back, and a
companion that joins as the bottom screen connects it again on its own.

**Edit Button Mapping** is the same page as a DS game's Change Controls,
for the controller the game has the player as: each of the pad's buttons,
what it presses on that controller, and the controller drawn beside the
list with that button filled in. A Wii Remote is held upright -- A and B as
they are, 1 on X and 2 on Y, the Nunchuk's C and Z on L and ZL -- and the
GamePad, a Pro Controller and a Classic Controller have their buttons
straight across. **A** picks another button for one, or one of
Dualscreen's own -- **Open Menu**, **Swap Screens** (as the menu's), or
**Peek**, which shows each device the other screen while it is held and
changes nothing else -- or Disabled; HOME is not offered, as a pad's own
reaches the menu; **Y** puts it on turbo; **X** puts every button back.
After the buttons come **Left Stick** and **Right Stick**: what each of the
pad's sticks moves -- the controller's own sticks (a Nunchuk's one), its
D-Pad, or nothing. A Wii Remote alone's left stick is its D-Pad, turned
with it when it is held sideways. Each player
keeps a mapping for each controller type, with the game, so changing a
player's type and back keeps what it had. Dualscreen does the mapping and
hands Cemu the controller's own buttons, so Cemu's own input settings for
these controllers are not used. A Wii Remote alone has **Sideways Wii
Remote** last: on, it is drawn on its side, the D-Pad on the left as on an
NES controller, and the D-Pad's directions are as it is held that way -- a
button on Up presses the remote's Right, which points up. What each button
is chosen as stays the same, and a profile keeps whether it is on. The
Controls page draws a player's Wii Remote on its side too.

**Save Profile…** and **Load Profile…**, at the top right of that page or
of a DS or 3DS game's Change Controls -- **Right** from the list -- each open
a page of mappings kept under a name, for any game to load. Save Profile's
**New Profile…** names the mapping as it is now and keeps it, and a profile
there is saved over, after asking. Load Profile's **A** loads one into this
game and **X** deletes one, after asking; the one the mapping is now says
**In Use**, and its name is under the two buttons. There is a list for DS
games, one for 3DS games and one for each Wii U controller type, so a Wii
Remote's profiles are only offered for a Wii Remote. A loaded profile is
copied into the game, so changing the game's mapping afterwards leaves the
profile as it was. They are kept by the device running the games, in
`mapping_profiles.json` beside its settings.

**Profile Switching**, on a Wii U player's page or the GamePad's, is **Off**,
**Hold −** or **Hold +**, and while it is on, **Switch Between** has a switch
for each of the controller's profiles. Holding the button half a second
moves the controller on to the next profile switched on, in the order they
were saved, round from the last to the first, as Steam Input's long press
does: a tap still does the button's job, reaching the game as it is let go.
The game keeps it for each player, with which profile each controller has.
Switching puts an alert in the corner, as a Switch's own do: the
controller, its player lights and the profile's name -- several at once
stacked down the side, on both devices.

In Controls itself, **X** (Identify) buzzes the highlighted controller, and
**Y** is **Change Order**, as on a Switch: every controller is let go of, and
a button pressed on each takes the next place -- the GamePad first, then
Player 1 onward -- with a click and a short buzz on that controller. The row
waiting for a press is filled in and says **Press a button**. A press after
every player has a controller adds another, up to eight, as the one before
it. **A** on a controller already taken is **Done**; **B** puts the old order
back. A GamePad the other device is holding stays with it, and the order
starts at Player 1. Any controller can be the GamePad, on either device.

Each row starts with what that controller is: the GamePad, drawn as one, or
a player, drawn as four small squares the way a Wii Remote's lights show it
-- the second square alone filled for Player 2, as a Wii U lights them,
rather than the first two a Switch would. A Wii U stops at four; Players 5 to
8 take a Switch's patterns. Over a player's lights is what the game has it
as, drawn as that controller -- a Wii U Pro Controller, a Wii Remote alone
or with a Nunchuk to its left, or a Classic Controller Pro -- the way
the back of a Wii U game's box shows them (flanx's drawings), and the same pictures are beside
each choice of Controller Type. A Switch Lite has no rumble motor, so its own controls cannot buzz. It all
works mid-game and from either device's buttons, and lasts until the app
closes: a controller is recognised for as long as it stays connected. As on
a Wii U, one leaving leaves its place empty -- Players 1 and 3 can play with
no Player 2, and a GamePad whose device has gone has no controller -- and
nobody moves up into it: a controller arriving takes the first empty place.

A player's rumble goes to its controller, on whichever device it is on. A
companion older than this needs updating to have its controllers counted one
by one. Until then it counts as one controller.

Each DualScreen controller's **Settings** button in Cemu offers motion, rumble
on or off, and a deadzone and range for each stick. On a Wii Remote, the
default mapping puts 1 on the north face button and 2 on the west one.

### Motion

A GamePad has a gyroscope and an accelerometer, and plenty of Wii U games steer,
aim or look around with them. Whichever device is holding the GamePad screen is
the thing being tilted, so that device's own sensors are what the game reads --
a Steam Deck's, a phone's, a Switch's, or the ones in a pad plugged into the
machine running Cemu.

A 3DS game reads motion too, but only with Azahar (Citra has no motion
support): whichever device holds the bottom screen is the one being tilted,
fed by the same sources as a Wii U's GamePad.

Only on a console that reads it. A DS session never switches a sensor on at
all, which on a handheld is a gyroscope not running for an hour for nothing.

Cemu reads it through the same **DualScreen GamePad** controller as everything
else (see above), with motion switched on for it by default. A player's motion
comes from the controllers behind it, where they have sensors SDL can read.
That means a controller plugged into the companion, since SDL reads the
companion's own pads directly. Under Steam Input, this device's controllers
have none (see below).

`motion: the host reads it; 1 of 1 connected controllers report it` in the log
is the line that says it is working. **`0 of 1` on a Steam Deck almost always
means Steam Input**, which re-presents the Deck's own pad as a virtual
controller with no sensors on it. Launching from desktop mode, or turning Steam
Input off for this app, gives SDL the real pad and its gyro with it.

If neither is convenient — and inside Game Mode neither really is — install
[SteamDeckGyroDSU](https://github.com/kmicki/SteamDeckGyroDSU). It reads the
Deck's real sensors and republishes them over DSU (cemuhook), the small UDP
protocol emulators already speak for exactly this problem. Nothing needs
configuring here: when no sensor on the device itself answers, this looks for a
DSU server on `127.0.0.1:26760` and uses it if one is there, logging `motion:
reading it from a DSU server at ...` when it finds one. Set
`DUALSCREEN_DSU_SERVER` to point somewhere else (`host`, `host:port`, or `off`
to skip looking) — which is also how to use a phone or anything else that
serves DSU as the motion source.

### Web Play

With **Settings > Wii U > Web Play** on, anyone on your network (or on a VPN
such as Tailscale with you) can watch and play the Wii U game from a web
browser, with nothing to install. The row shows the address to open. They
type a name and join: they see the TV and hear the game, and the controllers
on their device become players in Controls and Change Order, under their
name. A phone with no controller gets touch controls.

Whoever's controller is the GamePad is shown the GamePad's screen instead,
and touching it is the GamePad's touch screen. Passing the GamePad passes the
screen with it. The first time, a browser warns about Dualscreen's own
certificate; choose to continue. [docs/web-play.md](web-play.md) covers
certificates, playing over the internet and bandwidth.

## Settings

Settings is laid out as Graphics Packs is: its groups down the left, and the
rows of the one highlighted on the right. Right or A goes into a group, Left or
B back to the list. A group is the same on every device; a row a device has no
use for is left out rather than moved, and a group with nothing in it is not
listed.

| Group | Rows |
| --- | --- |
| General | Controller Icons and Confirm/Cancel (not on a Switch), Touch Controls (Android, iPhone, iPad), Picture in Picture (iPhone, iPad), Streaming Quality and Vsync (on a device that plays games), Sound Effects |
| DS & 3DS | Nickname, Favorite Color, Birth Month, Birth Day, Run Ahead; then melonDS's own settings where it is built in (CPU Clock While Playing, Each Frame Shown) |
| Wii U | Default Wii U Screen, Pause Wii U Games in the Quick Menu, Console Language, Wii U TV Audio, Wii U TV Volume, Wii Remote Rumble Strength, Async Shader Compile, Intro Sound, Web Play, Download/Update Graphics Packs; then Notifications (Controller Profiles, Low Controller Battery, Shader Compiling, Friend List) |
| System | Discord Rich Presence (PC and Steam Deck), Quick Start, Enable Remote Start (Linux, Steam Deck, Windows), Save Data Management; then Check for Updates, the build of Dualscreen this is, each emulator it keeps, and a Download row for each it does not have yet |

**Touch Controls** puts an on-screen pad below the game on a phone or tablet
held upright with no controller connected: a d-pad, the face and shoulder
buttons, Start and Select, and a 3DS's circle pad or a Wii U GamePad's
sticks. A finger on a control is that control's; every other finger still
touches the game. **Picture in Picture** keeps a streamed game playing in a
floating window when you switch to another app.

**Discord Rich Presence** shows the game you are playing, and for how long, on
your Discord profile.

### Emulators

On a PC or a Steam Deck, Dualscreen can download its own copy of each
emulator from **Settings > System** (a **Download** row for each one not here
yet), or when you first start a game that needs it, and keeps it up to date from then on: **Check for Updates**
looks at every one it has, and the check at start-up at the ones in use,
offered the same way as Dualscreen itself:

| Console | Emulator | From | Kept in |
| --- | --- | --- | --- |
| DS | melonDS DS | libretro's buildbot, as RetroArch downloads it | `~/.config/dualscreen/cores/` |
| 3DS | Azahar (Citra where the buildbot has no Azahar) | libretro's buildbot | `~/.config/dualscreen/cores/` |
| Wii U | Cemu, this project's fork | the fork's nightly (`joshbirnholz/cemu-dualscreen`) | `~/.local/share/dualscreen/cemu/` |

A downloaded emulator becomes the one its console is played with (the core
path, or `wii_u_program`, in settings.json); one of your own is used until then.
Updating one later leaves whichever is in use as it is. A buildbot core is told apart by the
date and hash in the buildbot's index, which is kept beside it as
`<core>.version`. On Windows they are kept under `%LOCALAPPDATA%\dualscreen`
instead. A Switch's DS emulator is built into Dualscreen, and an Android
device's or an iPhone's comes in its app, so those are updated with Dualscreen
itself.
