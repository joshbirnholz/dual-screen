# Dualscreen

A shell for playing dual-screen games on Linux/SteamOS that treats each screen
as its own addressable source, so you pick a screen layout from a visual menu
instead of cycling a button — and the choice is remembered per game.

Two devices can share one game: whichever device runs it serves a screen over
the network to the other, which sends its input back. Either device can be the
one running the game.

![the launcher](docs/images/launcher.png)

![the layout picker](docs/images/layout-picker.png)

![the game browser](docs/images/game-browser.png)

## What it does today

- Plays **Nintendo DS** and **Nintendo 3DS** games, picking the console from
  the ROM's extension and loading that console's core — including cores that
  render on the GPU, which is the only way Citra runs at all.
- Embeds the libretro core directly — **melonDS DS** for DS games, **Citra**
  (or a fork) for 3DS ones — `dlopen`'d as a library, with the libretro
  frontend implemented here. RetroArch is not involved and does not need to be
  installed.
- Pins the core to a single known internal layout and splits its framebuffer
  into two textures — top screen and bottom screen — every frame.
- Composites those two textures into **your chosen layout**, which is entirely
  independent of whatever the core is doing internally.
- Runs as one fullscreen window, so it behaves like any other non-Steam
  shortcut under gamescope / Game Mode.
- Controller input via SDL2's GameController API; touch input on whichever
  screen is currently showing the bottom screen, mapped back into the
  console's own touchscreen coordinates.
- Remembers the chosen layout per game, keyed by the ROM's SHA-1 rather than
  its path, so moving your ROMs around loses nothing.
- Writes cartridge saves as `.srm` files, interchangeable with RetroArch's.
- **Two-device play**: one device runs the game and shows a screen; the other
  finds it on the network, shows the other screen, and sends its touches and
  buttons back.
- Launched with no ROM, it opens a file browser so you can pick one, and starts
  in whichever folder you were last in.
- Takes a **Wii U**'s two screens from an emulator running beside it rather than
  from a core inside it, and treats them like any other console's from there.
  Finished and tested on this side; no emulator speaks it yet.

## Layouts

These are the Wii U Virtual Console's own Screen Settings, in its order and
under its names — that list is what this app is trying to be.

| id | what it shows |
| --- | --- |
| `standard` | both screens inside a drawing of the console itself |
| `upper_focus` | the upper screen large, the touch screen small in the corner |
| `touch_focus` | the touch screen large, the upper screen small in the corner |
| `vertical` | the DS's arrangement without the console around it |
| `side_by_side` | held sideways: both screens turned a quarter turn |
| `upper_on_tv` | one screen per device, each filling its own display |

The arrangements are measured off the Wii U's own output rather than eyeballed:
the sizes, the gaps and the alignments all come from its Screen Settings shots.
In the two focus layouts the small screen sits where that screen sits on a real
DS — the touch screen low, the upper screen high — so one pair is bottom-aligned
and the other top-aligned.

The two focus layouts are the same pair of screens with the emphasis reversed,
and which one you want changes with what the game is doing, so **L3** swaps
between them while playing — no menu, no pause. It does nothing under any other
layout, and like everything else on that menu it is one choice for both devices:
pressing it on the companion asks the host, the same as picking the layout from
the menu would. **F3** does the same from a keyboard.

**Hands** in the menu mirrors the layouts that put one screen beside another, so
the touch screen is under whichever hand holds the stylus. It is one toggle
rather than a left-handed twin of every layout, and it turns the sideways
screens the other way too.

`vertical` and the two focus layouts have a **Gap** toggle in the menu, on by
default, for the space between their screens. Side-by-Side is the one
exception — its gap stands in for the DS's own hinge, so it stays fixed —
but everywhere else, screens flush against each other are a real option, not
a broken one, so the gap is left up to the player. The layout previews update
live as the toggle changes, the same as picking a different layout does.

On a display no bigger than a Deck's — a Switch's 1280x720 included — `standard`
fills it with the **screens** rather than with the picture around them. Fitting
the whole console in leaves the screens at a fraction of the display with the
rest given over to plastic and grey: measured on a 1280x720 panel, a 3DS's two
screens came to 89% of the height of a picture only 694 pixels wide. The console
is scaled up until the screens span the display and the artwork simply runs off
the top and bottom, which is what a handheld held in two hands should look like.
A display with room to spare still gets the whole console, uncropped.

`standard` draws an illustration of the console itself and puts the two screens
exactly where its own are, on a #252525 surround rather than black — that is the
artwork's own background colour, so the picture has no visible edge. There is one
picture per console, a DS and a 3DS, and the game being played decides which is
drawn. See [THIRD_PARTY.md](THIRD_PARTY.md) for a note about where those images
came from.

Both are scaled to fit whatever display they are drawn on, the same picture
the Wii U Virtual Console's own Screen Settings shows at any size — filling
the screen rather than sitting pinned to the artwork's own pixel size on a
small display.

Every layout works on both consoles. A 3DS's two screens are different shapes —
400x240 above, 320x240 below — and each layout measures each screen against its
own size rather than assuming they match, so the wider upper screen stays wider
and the narrower touch screen stays centred beneath it — `standard` included,
which has its own 3DS to draw them into.

`upper_on_tv` is the two-device layout: each device fills itself with the screen
it owns. Pick any other and both devices show the whole thing, which is what you
want when someone is watching over your shoulder. With no second device
connected it falls back to showing both screens, since half a game is not
useful.

## Wii U games

A Wii U is not played the way a DS or a 3DS is. Those load a libretro core into
this app; there is no usable Wii U core to load, so a Wii U is played by **Cemu,
running beside this app**, handing its two pictures over.

Everything on this side of that handover is finished: the shell opens a socket,
takes a television screen and a GamePad screen from whatever connects, arranges
them under the same layouts as any other console, and can send one of them on to
a second device.

The Cemu that speaks it is a fork — `joshbirnholz/cemu-dualscreen`, branch
`claude/dualscreen-frame-link`. It taps the frames where Cemu's own screenshot
path is tapped, reads them back without ever waiting for the GPU, and takes the
whole GamePad from the link while the shell is connected. Vulkan only.
Confirmed against a real session on a Steam Deck: video, pad, touch and both
TV and GamePad audio all working. `docs/cemu-integration.md` is the full
account, including the one remaining gap — Reset — and why it is where it is.

`tools/fake_wii_u` stands in for it in the tests, which is what
`tests/run_wii_u_test.sh` and `tests/run_wii_u_companion_test.sh` drive.

The shell starts the emulator itself, from a command you configure. That is not
a convenience: in Game Mode there is no desktop to start one from, which is the
whole reason this app exists. In `settings.json`:

```json
{
  "wii_u_program": "/path/to/cemu",
  "wii_u_arguments": ["--dualscreen-socket", "{socket}", "--game", "{game}"]
}
```

Those arguments are the default, because they are the fork's; only the program
has to be set. They are written out in full because nothing here knows what a
different fork would call its own flags. `{socket}` becomes the path it should
hand its screens over on and `{game}` the file you picked; everything else is passed through
untouched, with no shell in between. Quitting the shell stops it again.

The Settings screen lists it beside the DS and 3DS cores, so a machine that is
not set up for a console says so before you pick a game rather than after.

Leave `wii_u_program` empty to start it yourself — which works on a desktop, and
does not work from a Steam shortcut. Either way, a device sitting there before
the emulator exists says so rather than showing a blank window: the host names
the socket to point at, and the second device says which machine it is waiting
on.

Three things are different about a Wii U session:

- **One device shows one screen.** The television is the game and the GamePad is
  a second view of it, so squeezing both into one window is not the arrangement
  the console has. With a second device connected, each holds one. Alone, the
  device is whichever screen **This device alone is a Wii U's** on the Settings
  screen says — and Cemu is told, so its own window shows that screen rather
  than whatever it was last left on. **F3** swaps it for the rest of a session
  without writing anything down.

  That setting is **Auto** by default. On a Steam Deck's own 1280x800 panel
  the device is the GamePad; on any other display it is the television. So a
  Deck in the hand is the GamePad and the same Deck on a dock is the
  television. It is decided when the game starts. A Deck's display does not
  change size under a running app, so docking takes effect the next time
  Dualscreen is started. **Television** and **GamePad** pin
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
  "Playing on Cemu" notice: **Controllers**, **Settings**, **Disconnect** and
  **Quit**. It answers only while its window has the focus, so a press meant
  for the game never picks Quit. There is no Resume: the game never paused,
  and going back to it is switching back to Cemu's window.

- **None of the controller shortcuts exist.** A Wii U game reads every control
  the console has, including both stick presses, so nothing is taken from the
  pad — no L3, no R3, no chord, no two-finger tap. The menu is always on
  screen on the device running the game (see above), and **F1**, **Tab** or
  **Escape** on a companion; smoothing is **F2**, and Steam Input can be pointed at any of them. Which is
  also why every button on the pad, and both triggers as ZL and ZR, go straight
  through to the game — including from the second device, whose touches arrive
  as touches on the GamePad's own screen.
- **Screens go out as H.264 by default, rather than as changed tiles.** A
  GamePad screen in motion measures at 42.8 Mbit/s as tiles and 2.45 as video —
  see **What streaming a screen costs** below. If the other device cannot
  decode it, the session says so rather than falling back; **Streaming format
  (Wii U)** on the Settings screen switches it to tiles.

Cemu owns everything about the emulation: the resolution it renders at, its own
controller bindings, its own saves. The shell offers no render scale for a Wii U
because that setting is not its to offer.

### Controls, and where they are configured

Whatever is holding the GamePad screen appears in Cemu's own **Input Settings**
as a controller called **DualScreen GamePad**, under an API called
**DualScreen**. Add it to Player 1's Wii U GamePad the way you would any other
pad. It is listed whether or not a session is running, so a game's controls can
be set up in advance.

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

Keyboards are not in the line at all. Map a keyboard in Cemu directly, with
Cemu's own **Keyboard** API, if a game wants one.

Controllers start out in each device's own order, which under Steam Input is
Steam's. Each is shown with the name Steam gives it, where there is one, and
which device it is on and which player slot that device has it in, such as
"steamdeck P2" or "Switch Lite P1". Set `device_name` in settings.json to
change what this device is called there. The name is only shown: a virtual pad's name is fixed
when it appears, and after controllers come, go and are reordered it can
describe a different controller from the one behind it.

**Controllers**, in the quick menu, rearranges the line the way SteamOS's own
Controller Order screen does. Press confirm to pick a controller up, move it
up or down, and press confirm again to put it down. Moving one up from Player 1
makes it the GamePad; moving one down past the last player puts it in **Not
used**. Press **X** or **Y** (Identify) to give the highlighted controller
one short buzz, which is how to tell them apart. Either works, because which of
them a pad calls X depends on the pad. A Switch Lite has no rumble motor, so
its own controls cannot buzz. It works mid-game and from either device's
buttons, and lasts until the app closes: a controller is recognised for as long
as it stays connected, so one that is unplugged and plugged back in joins the
end of the line.

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

What crosses the link is raw: rotation in radians per second and acceleration in
metres per second squared, exactly as the sensors report them, with the moment
each was taken. Nothing here turns that into an orientation, because Cemu
already does, with sensor fusion tuned to what a real GamePad reports. Sending
it a second opinion about which way is up would be worse than sending it none.

On a Switch, which of its six-axis axes is which is not documented anywhere and
was got wrong repeatedly, so it is no longer asserted: the app works it out from
the console's own accelerometer, since the direction the ground lies in turns
with the console exactly as fast as the console turns. It settles after a few
seconds of ordinary handling and says what it found in the log. Until then the
last known-best guess stands, which is why a console left flat on a table keeps
whatever it started with.

**Switch gyro axes** in Settings turns that by hand instead, through all
forty-eight possibilities, taking effect immediately so it can be walked while
a game is running. It is temporary, and once the answer is known it belongs in
the code as a constant.

And when a device whose motion is already correct is hosting for one whose
motion is not, holding the two together and moving them as one makes the host
log both readings side by side and work out what stands between them — see
`MotionComparison`. Two readings of one motion settle in a minute what
reasoning about conventions did not settle in seven attempts.

A 3DS reads it too, through libretro's own sensor interface
(`RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE`) — but **only on `azahar_libretro`**.
`citra_libretro` has no motion support at all (one TODO in its source and
nothing else), so a 3DS gyro cannot work on it however much this side
implements. Azahar asks for the interface, has motion switched on by default,
and is looked for first. Whichever device holds the bottom screen is the one being
tilted, exactly as for a Wii U, and the same three sources feed it: a pad's own
sensors, the machine's, or a DSU server. libretro documents the units (g, and
radians a second) but never says what its sensor axes mean, so in practice they
mean whatever the core reading them assumes — here, the 3DS's own frame, since
Azahar passes them to its motion device untouched. Azahar's SDL path defines
the difference and is what this mirrors.

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

## Playing across two devices

Start the game on whichever device you want to run it — a gaming PC, or the
Deck. It announces itself on the network automatically.

On the other device, launch dualscreen with no ROM and pick **Connect as top
screen** or **Connect as bottom screen**. It finds anything running nearby,
lists it, and joins. The device holding the bottom screen is the one whose
touchscreen works, and both devices' buttons drive the game.

The layout menu opens on either device — **F1**, or a two-finger tap — and shows
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

**Settings** is not on that menu at all, on either device. What is in there —
folders, the core, what a new game starts with — is setup, not something to
reach for mid-game, so it lives on the launcher: before you load a game or
connect to anything.

```sh
# Skip the menus entirely, for a Steam shortcut on the companion device:
dualscreen --connect bottom
```

### A Switch or an Android handheld as the second screen

The other device does not have to be running this app, or be a PC at all.
The **companion** does one thing: join a game running somewhere else and be one
of its two screens. It has no emulator in it and needs no ROM, no BIOS and no
core — everything is being played on the other device.

It is one program, in `companion/`, built for four kinds of device:

- a **Nintendo Switch**, as homebrew — see [switch/README.md](switch/README.md)
- an **Android** handheld, phone or tablet — see
  [android/README.md](android/README.md); aimed at a Logitech G Cloud
- a **Mac**, as an ordinary application — see
  [apple/README.md](apple/README.md), which is also honest about the fact that
  it has not been run on one yet
- this **desktop app** itself, with *Connect as top/bottom screen*

It shares the parts that describe a session — the protocol, discovery, the
layout arithmetic, the controller mapping — with this app rather than
reimplementing them, so a host cannot tell the difference between it and
another PC, and neither can the menu.

Its in-session menu *is* this menu — the same source, the same tiles, the same
previews, the same footer — because the device showing a screen is usually the
one being held, and the alternative is reaching for a pad plugged into the
other machine. The only entry it drops is **Pointer**, which is the one thing
on that menu belonging to the device rather than the host, and a console has no
arrow to show or hide.

## What streaming a screen costs

A DS screen is 256x192. In RGB565, deflated, one frame of *moving* content
measures around 40 KB here — about **20 Mbit/s at 60fps, per screen**. A layout
that puts both screens on the companion doubles that. That is a lot to ask of a
handheld's Wi-Fi, and it is why a mostly-static screen streams fine while a
scrolling one struggles.

Three things keep it as low as it can go:

- **Only the tiles that changed are sent.** A screen is cut into 16x16 tiles
  and a frame carries a bitmap of which ones differ from the last frame that
  device was actually sent. A screen where a marker blinked measured 809 bytes
  against 42 KB for the whole frame; a screen that scrolls changes every tile
  and costs what it always did, within a couple of percent.
- **A frame is only sent when the picture actually changed**, and a frame that
  cannot be sent promptly is dropped rather than queued, so a device that
  cannot keep up falls behind in frames rather than in seconds.
- **The pixels stay in RGB565 all the way to the texture** on a device whose
  renderer can upload that, which is a whole pass over every frame and half the
  bytes to the GPU that never happen.

**Which of the two a session uses** is a setting per console: **Streaming
format (DS)**, **Streaming format (3DS)** and **Streaming format (Wii U)** on
the Settings screen, each Tiles or H.264. DS and 3DS default to tiles, Wii U to
H.264 — the answer each measures better at, from the numbers above. `--video-codec
tiles|h264` overrides whichever of those the game being launched would have used,
for one run, without editing anything.

There is no fallback between the two. Asking for H.264 needs a real answer from
both ends — an encoder here, a decoder at the far end — and if that answer is
no, the session says so on screen rather than quietly sending tiles instead: a
picture that arrived by the other path would say nothing about why the one you
asked for did not work, which defeats the point of being able to ask for either
on purpose. A build without FFmpeg, or a companion that cannot decode it, is
what a video streaming error names.

If a companion still cannot keep up, in order of how much they buy:

1. **Use the Upper Screen on TV layout.** One screen per device rather than
   both on each: half the bandwidth, immediately.
2. **Get it onto a wire.** A USB Ethernet adapter in a Switch's dock is 100
   Mbit/s with none of the jitter, and needs nothing from this app — it is the
   same TCP either way. Docked Wi-Fi is usually better than handheld too.
3. **Turn the render scale down** on a 3DS game. A screen streamed at 3x is
   nine times the pixels of one at 1x.

## Smoothing

A DS screen is 256x192 and a Deck's is 1280x800, so every pixel is drawn several
pixels wide. **Smoothing** in the layout menu chooses how: off (the default)
keeps the original pixels square and sharp, on blends between them. **R3**
toggles it from anywhere — in a game, in the layout menu, in Settings — so the
difference can be judged against what is actually on screen rather than from
memory. A game remembers which it was last given; the setting a game starts
from is `"default_bilinear_filtering"` in `settings.json`.

It applies to streamed screens too, and while two devices are connected it is one
choice for both of them, like everything else on that menu.

It is a request rather than an instruction, and what happens to it depends on
how the screen actually lands. One texture pixel per display pixel is drawn
untouched whatever the setting says — there is nothing to resample, and sampling
it anyway could only soften it. That still happens wherever a screen's own
arithmetic lands it exactly on its own pixel size — the small screen of either
focus layout does this at some window sizes — but it is caught wherever it
happens rather than something any one layout is built around.

A screen with *more* texture than there is room for is always resampled, setting
or no setting: that is what a core rendering at 2x into a smaller rect produces,
and point-sampling a reduction throws away exactly the pixels the higher
resolution was paid for. "Keep it crisp" is a statement about being magnified,
which is the case it was asked for.

Screens go over the wire as deflated RGB565, which measures around 1.3 Mbit/s
for both screens at 60fps on DS-like content — no video encoder involved, and
comfortably within wifi. Only screens the current layout actually needs are
sent, and an unchanged frame is not sent at all.

A 3DS game is served the same way, but it takes more work to get there: Citra
renders on the GPU, so there is no copy of the frame in memory to send. Every
screen streams at the console's own resolution — 400x240 and 320x240 —
regardless of the render scale, which stays a local sharpening on the device
running the core rather than something a companion also pays for. A render
scale above 1x means more to read back off the card and pack down for the
wire, and deflated RGB565 is a general-purpose byte codec, not a video one:
its cost is proportional to the pixel count a render scale multiplies, and at
2x or 3x that made for a choppy stream rather than a sharper one. Streaming a
companion more than the console's own resolution when it can use it — a
companion's window is usually bigger than 400x240 — is worth doing, but not
with this encoder; it needs a real hardware video pipeline (encode on the
GPU, decode on the GPU, an actual codec bitstream) to be worth the bandwidth,
which is future work.

The send queue's backlog limit follows the same reasoning: it is measured in
frame-times (roughly three frames' worth of whatever was last actually sent)
rather than a fixed byte count, so a companion that cannot keep up sees
dropped frames whether it is looking at a DS's picture or something several
times the size, instead of a limit tuned for one and silently wrong for the
other.

Adding a layout is meant to be adding data, not code. A layout says how its
screens are *arranged* — along which axis, at what relative sizes, how far apart,
aligned to which edge — and the rectangles are solved for at draw time
(`src/layout/layout_registry.cpp`). That is what lets the same description serve
a 3DS, whose two screens are different sizes and shapes, or a Wii U's TV and
GamePad: new data rather than new arithmetic. Touch mapping reads the same
solved rectangles — including any quarter turn, inverted — so a new layout gets
working touch for free, sideways or not.

## The right stick, on 3DS games

A 3DS has a circle pad of its own, and the handful of games built for a Circle
Pad Pro or a New 3DS read a second stick as well. Nothing in a ROM says which,
so **Right Stick** in the layout menu cycles between:

- **C-Stick** — a Circle Pad Pro / New 3DS second stick. The default.
- **Circle Pad** — a second way to drive the circle pad, alongside the left
  stick. The stronger of the two wins, so pushing either one walks.
- **D-Pad** — pressed directions, the way the left stick works on a DS.
- **Off** — ignored entirely, for a game that does something unwanted with a
  stick it is handed.

Each mapping reaches exactly one destination. A stick feeding two at once is
what made the circle pad unusable back when the left stick was also the d-pad:
a core that reads the d-pad sees a direction held down for as long as the stick
is off centre.

The button appears only on a console with a stick of its own — a DS's left
stick is already standing in for its d-pad and the right one has nothing to be.
A game remembers what it was last set to; the value a new game starts from is
`"default_right_stick_mode"` in `settings.json`. Like everything else on that
menu it is one choice for both devices, since both devices' pads drive the same
game.

## Render scale

A 3DS renders at 400x240 and 320x240. Citra can draw the same frame at a
multiple of that, and **Render** in the layout menu cycles between **1x**, **2x**
and **3x** while the game runs. It changes how much the emulator draws and
nothing else: the screens stay exactly where the layout puts them, at the same
size, and only get sharper. A game remembers what it was last set to; the value
a new game starts from is `"default_render_scale"` in `settings.json`.

2x on a Deck's 1280x800 screen is the interesting one — the upper screen becomes
800x480, which is most of the width it is drawn at, so the softness of an
upscaled 400-pixel picture goes away. 3x fills a 1080p TV from the upper screen
alone. Citra offers up to 10x; the menu stops at 3x because past that a handheld
GPU pays for pixels nothing can show.

It is shared like everything else on that menu. Only the host is running a core,
but the menu is one menu reachable from either device: a companion shows the same
button, and pressing it asks the host, which changes the core and broadcasts the
result back. Neither device decides it locally, so the two cannot disagree.

A DS game does not have the button at all: melonDS is pinned to its software
renderer, which draws one pixel per DS pixel and has nothing to multiply.

## Building

Every platform builds from this one repository. The desktop app is the full
thing: it runs games and can also join one as a companion. Everything else in
the table only joins a game running somewhere else.

| Platform | What you get | Command | Details |
| --- | --- | --- | --- |
| Steam Deck / SteamOS | the desktop app | `./tools/build-steamos.sh` | [below](#steam-deck--steamos) |
| Linux, other desktops | the desktop app | `cmake -S . -B build && cmake --build build -j` | [below](#linux-and-other-desktops) |
| macOS | the companion, as `Dualscreen Companion.app` | `cmake -S apple -B build-macos && cmake --build build-macos` | [below](#macos), [apple/README.md](apple/README.md) |
| iPhone, iPad, Apple TV | the companion | `apple/build-all.sh` | [below](#iphone-ipad-and-apple-tv), [apple/README.md](apple/README.md) |
| Nintendo Switch | the companion, as `Dualscreen.nro` | `cd switch && make` | [below](#nintendo-switch), [switch/README.md](switch/README.md) |
| Android | the companion, as an APK | `cd android && ./gradlew assembleDebug` | [below](#android), [android/README.md](android/README.md) |
| Cemu, for Wii U games | the fork this app hands screens over with | `cmake -S . -B build -G Ninja && cmake --build build` | [below](#cemu-for-wii-u-games) |

After `git pull`, run the same commands again, both configure and build. The
configure step picks up new files and records which build this is. Every build
writes its commit into the second line of its log (`this build is d5eb384 …`),
which is the quickest way to check that a device is running what you just
built.

### Steam Deck / SteamOS

```sh
git clone https://github.com/joshbirnholz/dual-screen
cd dual-screen
./tools/build-steamos.sh
```

That is the whole thing. First run takes a few minutes to fetch the container
image; after that it is seconds. The result is `build/dualscreen`, which runs
**on the SteamOS host** — in Game Mode, with no container at runtime, and it
keeps working across SteamOS updates. If FFmpeg was available to build
against, `build/` also holds a handful of `.so` files the binary needs for
H.264 — see below — so treat `build/` as the artifact to move around (to add
as a Steam shortcut from elsewhere, say) rather than `dualscreen` alone.

<details>
<summary>Why it works this way</summary>

SteamOS's root filesystem is read-only and ships no compiler, so the build has
to happen in a container. The script uses **Debian 12**, which is deliberately
*older* than any SteamOS release: a binary built against an older glibc runs on
a newer one, but not the reverse. Build in an Arch container instead — the
obvious choice, since SteamOS is Arch-based — and you get a rolling-release
glibc newer than the Deck's, and a binary that will not start.

That direction matters more than it sounds. Built on glibc 2.39, this binary
demands glibc ≥ 2.38, entirely because GCC 13 quietly rewrites `strtol` and
friends into `__isoc23_strtol`. Nothing in the source needs anything that
recent. Build it on Debian 12 and the floor drops to what the code actually
uses.

The other half is what the binary needs at all:

```
$ objdump -p build/dualscreen | grep NEEDED
  NEEDED   libSDL2-2.0.so.0
  NEEDED   libm.so.6
  NEEDED   libc.so.6
```

SDL2 and libc, and that is it. Two reasons:

- `libstdc++` is linked statically (`DUALSCREEN_PORTABLE`, on by default),
  since the C++ ABI is the coupling most likely to break when a container-built
  binary meets the host's runtime. Costs about 1.4 MB.
- **`libGL` is absent**, even though CMake links it. Every GL entry point is
  resolved through `SDL_GL_GetProcAddress` at runtime, so the linker drops the
  direct dependency and there is no GL/EGL version coupling to get wrong. This
  is the same property that lets the renderer take an OpenGL ES context on one
  machine and desktop GL on another.

FFmpeg is the one exception to "SDL2 and libc, and that is it," and it ships
no static archives the way libstdc++ does, so a binary built with H.264
support genuinely needs `libavcodec.so`, `libavutil.so` and `libswscale.so`
at runtime. Debian's own `libavcodec-dev` was the obvious way to get them,
but a stock build of it links dozens of codecs this project has no use for
— AV1, HEVC, Theora, JPEG XL (which alone drags in librsvg, libcairo and
glib) — every one of which the SteamOS host would then also need too, or
this script would have to bundle. `build_ffmpeg_if_needed` in
`tools/build-steamos.sh` builds FFmpeg from source instead, configured with
nothing but the h264 decoder/parser and the h264_vaapi encoder/hwaccel (see
**What streaming a screen costs**, and `src/video/video_encoder.cpp`/
`video_decoder.cpp` for what actually gets called). The result is three
small libraries whose own dependencies — `libva`, `libdrm`, `libX11` — are
exactly the "any Linux desktop with GPU acceleration already has this"
class of library `libGL` above is already left unbundled for; `libx264` is
deliberately not built at all, keeping this LGPL rather than GPL. Cached
under `.ffmpeg-steamos/` at the repo root (separate from `build/`, so
`--rebuild` does not force redoing a from-source FFmpeg build every time),
`bundle_ffmpeg_libs` copies just those three files into `build/` next to
the binary, and the binary's own `$ORIGIN` rpath (set in `CMakeLists.txt`
when FFmpeg is found) is what makes it look there before giving up. A build
without FFmpeg available to configure against copies nothing and needs
nothing extra, same as before.

**Do not use `sudo steamos-readonly disable`** to install a compiler on the
host. It works until the next SteamOS update wipes it, and then it works again
until the update after that.

</details>

### Linux and other desktops

Needs a C++17 compiler, CMake 3.16+, SDL2 and nlohmann::json.

```sh
sudo apt install build-essential cmake libsdl2-dev nlohmann-json3-dev  # Debian/Ubuntu
sudo pacman -S base-devel cmake sdl2 nlohmann-json                      # Arch

cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build --output-on-failure
```

If nlohmann::json is not installed, CMake fetches it. `build-steamos.sh` also
detects a non-SteamOS machine with a compiler and just builds directly, so it
is safe to use everywhere.

H.264 encoding (see **Which of the two a session uses**, above) needs FFmpeg's
dev packages found by `pkg-config` at configure time:

```sh
sudo apt install libavcodec-dev libavutil-dev libswscale-dev  # Debian/Ubuntu
sudo pacman -S ffmpeg                                          # Arch
```

Without them, CMake prints `video codec: not available, building without one`
and carries on -- a build like that still runs everything else, but a game
whose streaming format is set to H.264 shows a video streaming error on
connect rather than a picture, since there is nothing here to fall back to.

Useful CMake options: `-DDUALSCREEN_PORTABLE=OFF` (link libstdc++ dynamically),
`-DDUALSCREEN_BUILD_TESTS=OFF`, `-DDUALSCREEN_WERROR=ON`.

### macOS

The Mac build is the companion app. It joins a game running on another device
rather than running one itself.

```sh
brew install cmake sdl2 ffmpeg
cmake -S apple -B build-macos
cmake --build build-macos
open "build-macos/Dualscreen Companion.app"
```

**Quit the running app before rebuilding** (⌘Q, or `killall "Dualscreen
Companion"`). Otherwise `open` brings the old copy to the front, and it looks
as though the rebuild did nothing. For a universal binary, add
`-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"` to the first `cmake`. More in
[apple/README.md](apple/README.md).

### iPhone, iPad and Apple TV

Xcode projects are generated by CMake rather than committed.
`apple/build-all.sh` builds every Apple platform and puts the projects in one
workspace:

```sh
apple/build-all.sh                              # simulators for iOS and tvOS, plus macOS
apple/build-all.sh --device --team XXXXXXXXXX   # for real devices, signed with your team
open build-apple/Dualscreen.xcworkspace
```

Then pick a target in Xcode and press Run. A real Apple TV needs SDL built
for tvOS first; [apple/README.md](apple/README.md) has that, and the ways a
device build tends to fail.

### Nintendo Switch

Needs devkitPro with the Switch toolchain. On Windows, run this from
devkitPro's MSYS2 shell, without `sudo`.

```sh
pacman -S switch-dev switch-sdl2 switch-zlib
pacman -S switch-ffmpeg            # optional: H.264 video, found automatically
export DEVKITPRO=/opt/devkitpro
cd switch && make
```

That produces `switch/Dualscreen.nro`. Copy it to `/switch/` on the SD card
and start it from the homebrew menu. More in [switch/README.md](switch/README.md).

### Android

Needs the Android SDK and NDK, and SDL2's source, which is not vendored:

```sh
git clone --depth 1 --branch release-2.30.9 \
    https://github.com/libsdl-org/SDL android/app/jni/SDL
cd android
gradle wrapper          # once, or open this directory in Android Studio
./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

H.264 on Android needs FFmpeg built with the NDK; see
[android/README.md](android/README.md).

### Cemu, for Wii U games

Wii U games run in a fork of Cemu, `joshbirnholz/cemu-dualscreen`, branch
`claude/dualscreen-frame-link` (see **Wii U games** above). It builds the way
Cemu does, with the same dependencies; the fork's own `BUILD.md` lists them
for each platform. On Linux:

```sh
git clone --recursive --branch claude/dualscreen-frame-link \
    https://github.com/joshbirnholz/cemu-dualscreen
cd cemu-dualscreen
cmake -S . -B build -DCMAKE_BUILD_TYPE=release -G Ninja
cmake --build build
```

The result is `bin/Cemu_release`. Point `wii_u_program` in `settings.json` at
it (see **Wii U games**). SteamOS has no compiler on the host, so build it
inside a container or distrobox, the same way `tools/build-steamos.sh` does
for this app. After pulling new commits, run `git submodule update --init
--recursive` as well, since Cemu's dependencies are submodules.

## What you need to supply

**A core per console you want to play.** For DS, melonDS DS
(`melondsds_libretro.so`). For 3DS, Azahar (`azahar_libretro.so`) for
preference, or Citra (`citra_libretro.so`, `citra_canary_libretro.so`) —
Azahar is looked for first, is the maintained one, and is the only one with
gyro support. The two do not spell their options identically (Azahar's screen
layouts are `default` where Citra's are `Default Top-Bottom Screen`, and so
on); dualscreen asks each core which spellings it has rather than assuming, so
either works. Get them through RetroArch's core downloader, your distribution,
or the projects' own releases. dualscreen looks in the usual RetroArch core directories and
`/usr/lib/libretro`, and the Settings screen lists what it found for each
console. Override with the `core_path` and `core_path_3ds` settings, or with
`--core` for a single run.

A Wii U needs no core at all — it is played by Cemu running beside this app, and
`wii_u_program` says where. See **Wii U games** above.

You only need the emulator for the console you actually play; a missing one is
reported when you try to load a game for it, not before.

3DS emulation needs **OpenGL 3.3**. Citra has no software renderer: it asks for
a 3.3 context and refuses to load a game without one, so dualscreen builds the
window for whichever console you are about to play. Any machine that can run a
3DS emulator at all has this; a driver that does not gets a plain error screen
saying so rather than a game that never starts. DS games are unaffected and
still run on the OpenGL ES 2.0 baseline.

**BIOS/firmware, if you want them.** melonDS DS boots DS games using its own
open-source BIOS replacement, so nothing is strictly required for DS. For
native boot or DSi mode, put your own dumps (`bios7.bin`, `bios9.bin`,
`firmware.bin`, and the `dsi_*` files) in the system folder. 3DS games need the
system files Citra asks for, which come off your own console. **No
Nintendo-owned data ships with this app and none is downloaded** — these have
to be your own. The Settings screen shows exactly which DS files it can and
cannot see.

**ROMs.** Yours, obviously.

## Running

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

Full options:

```
--game <path>        ROM to boot straight into
--core <path>        libretro core (.so) to load, instead of the one found
                     for this game's console
--layout <id>        Override the layout for this run
--system-dir <path>  Folder holding your BIOS/firmware files
--windowed           Run in a window instead of fullscreen
--list-layouts       Print the available layout ids and exit
--verbose            Log core and frontend debug output
```

### In-game controls

| | |
| --- | --- |
| Open/close the layout menu | **L3 + R3**, **F1**, or a **two-finger tap** |
| Smooth or sharpen the screens | **R3** alone (or **F2**), in a game or in any menu |
| Swap the two focus layouts | **L3** alone (or **F3**), while a game is on screen |
| | *DS and 3DS only. A Wii U game reads both sticks pressed and plays through its touch screen, so none of the three shortcuts above are taken from it — not L3, not R3, not the pair, and not the two-finger tap. There the menu is **F1**, **Tab** or **Escape** alone, which Steam Input can be pointed at, and **F3** swaps which screen a lone device is showing.* |
| Open Settings, from inside that menu | **−** (or **F4**), on every console — it is a menu shortcut rather than one taken from the game. The settings are whichever device is running the game, whoever asked; with two connected, the other one says to look over here and its buttons still work the screen |
| Move with the circle pad | the left stick, on 3DS games — where it is the circle pad and only that |
| Move with the C-Stick | the right stick, in the handful of 3DS games built for a Circle Pad Pro or New 3DS |
| Move with the d-pad | the d-pad; on DS games, which have no stick, the left stick works too |
| Touch the touchscreen | tap or drag on whichever screen is showing the bottom screen — a tap that misses the screen itself lands on whichever edge it's nearest to |
| Point with a mouse or trackpad | the pointer appears when you move it and hides again after two still seconds |
| Turn the pointer off entirely | **Pointer** in the layout menu |
| Move around the menus | d-pad, left stick or arrow keys, held down to keep moving; the east face button confirms, the south one goes back |
| Page through a long list | **L** and **R**, or drag the list with a finger |

### If it quits immediately in Game Mode

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

### Steam Input

Launched from Steam, your pad reaches the app through Steam Input, which
matters in three ways.

**Bind F1 to open the menu.** No controller button opens it: Steam claims the
Guide button, and anything else is a button a game might want. Add a keyboard
binding for **F1** to a spare input in the shortcut's controller layout — a
back paddle, a trackpad click, whatever you have spare — and that becomes your
menu button.

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

### Saves

Cartridge saves are written to `~/.local/share/dualscreen/saves/<rom name>.srm`,
the same format and naming RetroArch uses — copy an existing `.srm` in and it
will be picked up, or copy one out and RetroArch will read it.

They are written whenever the game changes them, checked every few seconds, and
also on losing focus, on opening the menu and on quitting. A handheld gets put
to sleep and powered off without warning, so waiting for a clean exit is not
good enough. Each write goes to a temporary file that is flushed to disk and
then renamed over the old one, so an interrupted write cannot leave a truncated
save behind.

DSi NAND and firmware saves are the core's own business and live in the system
folder.

### Face buttons

Everything — menus and game alike — uses **Nintendo positional** controls:

```
        north = X
   west = Y      east = A     confirms menus, and is DS A
        south = B             cancels menus, and is DS B
```

So on an Xbox-layout pad, which is what Steam Input's virtual controller is,
the button marked **B** confirms and is DS A, and the button marked **A**
cancels and is DS B. One rule in both places: the button that means "yes" in a
menu is the button that means A in a game.

To make that hold without guessing at each pad, SDL is asked to report face
buttons by *position* rather than by printed label. On-screen prompts still
name the letters your pad actually prints, so a Switch Pro pad reads "A: Select"
and an Xbox pad reads "B: Select" — the same physical button either way.

None of it is configurable. Remap the pad in Steam Input if you want something
else; that is a better place for it than a second set of bindings here. A pad
SDL has no entry for, or a wrong one, can be corrected without a rebuild by
dropping a `gamecontrollerdb.txt` next to `settings.json`.

The two-finger tap matters: on a tablet with no buttons attached, it is the
only way into the menu. Emulation pauses while the menu is open, and each tile
draws that paused frame in its own arrangement, so you are comparing layouts
against the game rather than against a diagram.

### Adding a Steam shortcut

Add `dualscreen` as a non-Steam game, then set the launch options to
`--game "/path/to/rom.nds"` — one shortcut per game. Each one boots into its
own remembered layout, which is the whole point.

V1 does not script `shortcuts.vdf` for you.

## Where things are kept

```
~/.config/dualscreen/settings.json          app-level defaults
~/.config/dualscreen/gamecontrollerdb.txt   optional controller mapping overrides
~/.config/dualscreen/games/<rom-sha1>.json  per-game layout
~/.local/share/dualscreen/saves/<rom>.srm   cartridge saves
~/.local/share/dualscreen/system/           where BIOS/firmware files are looked for
```

`settings.json`:

```json
{
  "default_layout_id": "standard",
  "last_browse_dir": "/home/deck/Emulation/roms/nds",
  "system_dir": "/home/deck/.local/share/dualscreen/system",
  "core_path": "",
  "core_path_3ds": "",
  "default_bilinear_filtering": false,
  "default_render_scale": 1,
  "audio_target": "both",
  "cursor_mode": "shown",
  "handedness": "right",
  "stick_deadzone": 0.35,
  "trigger_deadzone": 0.3,
  "vsync": true,
  "wii_u_alone": "auto"
}
```

`wii_u_alone` is `"auto"`, `"tv"` or `"gamepad"`, and starts as `"auto"`.
See **Wii U games** above. It replaces `wii_u_screen_alone`, which is no
longer read: every file that has it was written with the old version's guess
from the machine's shape, whether or not anybody had chosen it. Paths are edited here
rather than in the app: an on-screen keyboard is not something V1 builds. The Settings screen cycles between BIOS folders that
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
  frame plus their native resolution. Two implementations sit behind it now: one
  wrapping a libretro core, and one taking frames from an emulator running as a
  separate program. The compositor and the layout system never learn which.
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

- `test_layout` — placement geometry, the console shell keeping its proportions
  in any window, handedness mirroring a layout rather than duplicating it, touch
  mapping round-trips for every layout in both hands, quarter turns included, and
  when a screen is resampled on its way to the display rather than point-sampled.
- `test_config` — SHA-1 against the FIPS vectors, path handling, config
  round-trips including preservation of keys a future version might add.
- `test_browser` — which files the game browser offers, how it sorts and where
  it starts, the held-input gate that keeps a menu press out of the game, and
  that a duplicated press navigates once rather than entering a folder and
  backing straight out of it.
- `test_controller_mapping` — that the DS mapping swaps A/B and X/Y on an
  Xbox-layout pad and leaves a Nintendo-layout pad alone, that menus are
  unaffected by that setting, and that duplicate presses from two devices
  collapse to one menu action.
- `test_core_host` — loads a **mock libretro core** (`tests/mock_core`) and
  checks option forcing, geometry, video capture, the frontend's own validation
  of what the core accepted, and the cartridge-save round trip. Also checks
  that a screen read back for streaming is never sharper than what was
  actually rendered and never softer than the console's own resolution,
  whatever resolution is asked for.
- `test_quick_settings` — taps a grid over the layout menu and counts which
  taps do what, so the render-scale button is checked by being *found* rather
  than by being told where it is: present on a 3DS game, absent on a DS game and
  on a companion, and the six buttons beside it still each report their own
  intent now that the row wraps.
- `test_controller_input` — drives an SDL **virtual game controller** through
  the whole input path, so the face-button mapping is verified end to end with
  no hardware, and the left stick reaches the circle pad — pressing the d-pad
  too only on a console that has no stick of its own.
- `test_protocol` — every message round-trips, then the decoders are attacked:
  truncation at every byte offset, an absurd length field, and random-byte fuzz.
- `test_net` — real sockets over loopback: a beacon a browser receives, a TCP
  session carrying framed messages, a peer close reported as a disconnect.
- `test_session` — a real host paired with a real companion: that the split
  layout sends one screen and other layouts send both, that a companion is
  granted the screen it asked for, that audio arrives and stops when switched
  off, that a layout request reaches the host and its answer comes back, that
  the companion's reported window size reaches the host (and is unknown, not
  wrong, before it has said), and that either end can end the session.
- `two_device_headless` / `two_device_top_headless` — launch **two real
  processes** under `Xvfb`, one hosting and one joining, and check each ends up
  showing its own half. Run twice, once for each screen the companion can ask
  for, because those behaving identically was a real bug.
- `two_device_3ds_headless` / `two_device_3ds_top_headless` — the same pair with
  a host whose core renders on the GPU, which has to read each screen back off
  the card to send it. Reaching into the frame as though there were pixels there
  sent an empty top screen and then segfaulted the host on the bottom one; these
  catch that (the host exits 139) and, in the `top` case, that the colours
  survive the trip.
- `test_switch_client` — the Switch companion's own logic, which is plain C++
  and so runs here: the host list keeping its selection on the machine it was
  on as hosts appear and go quiet under it, and where a quarter-turned screen
  actually has to be drawn, which is off by a quarter turn everywhere except
  the one layout that turns anything.
- `switch_client_headless` / `switch_client_top_headless` — the Switch
  companion joining a **real host**, both processes under `Xvfb`, checking it
  ends up drawing the screen it asked for. The client is one program with a
  small guarded section for the console's own start-up, so everything under the
  window is the same code that runs on the Switch.
- `smoke_headless` — runs the real binary against the mock core under `Xvfb`,
  screenshots every layout, and checks pixel by pixel that each screen landed
  where the layout says. It also holds a synthetic touch and verifies the
  crosshair the core draws comes back **exactly under the touch point** — which
  only happens if the whole window → DS → pointer → framebuffer → split →
  composite path agrees. It then launches the binary twice against one save
  directory and checks the game starts fresh the first time and restored the
  second. The 3DS half of it runs every shared layout twice, at 1x and at 2x,
  and checks the screens land in exactly the same places both times — a render
  scale that moved anything would be a different feature. On the Standard layout
  it probes pairs of points either side of each screen's edge, so the game has to
  be inside the artwork's screen hole and plastic has to be just outside it.

The mock core means all of this runs with no melonDS core and no commercial ROM.

## Not built yet

A Wii U's Reset button reaching Cemu; see `docs/cemu-integration.md`, which has
confirmed-working video, pad, and both TV and GamePad audio against a real
session. Save states, cheats, rewind, shaders. More than
one companion per host. The companion in `companion/` is built and tested here as a
desktop program; the Switch build needs a devkitPro toolchain and the Android
build needs the Android SDK and NDK, so both have to be checked by hand on the
device. See `docs/research-notes.md`
for what was verified against the real melonDS DS core and what still has to be
checked on hardware.
