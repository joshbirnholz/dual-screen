# How Dualscreen works

The technical side of Dualscreen, for anyone working on it or curious about
it. For what it does and how to use it, see the [README](../README.md) and the
[guide](guide.md); for building it, [building.md](building.md).

- [How it fits together](#how-it-fits-together)
- [What streaming a screen costs](#what-streaming-a-screen-costs)
- [Latency](#latency), [screen smoothing](#screen-smoothing-and-sending-a-screen)
- [Motion](#motion)
- [Layouts](#layouts)
- [Menu sounds](#menu-sounds)
- [Where things are kept](#where-things-are-kept), [settings.json](#settingsjson), [the command line](#the-command-line)
- [Tests](#tests)
- [Not built yet](#not-built-yet)


## How it fits together

```
ScreenSource ──► two textures (top, bottom) ──► Compositor ──► window
   │                     per frame                  │
   │                                           OutputRouter
   │                                   (this window, a system video layer,
   │                                    or the other device)
   ▲                                                 │
   └──────────── InputSink ◄──── InputManager ◄──────┘
                 (buttons + touch in the console's own coordinates)
```

- **`ScreenSource`** (`src/core/screen_source.hpp`) produces two textures per
  frame plus their native resolution. One implementation wraps a libretro
  core (`LibretroCoreScreenSource`, melonDS DS or Azahar, `dlopen`'d and pinned to
  one internal layout); another takes frames from an emulator running as a
  separate program (`ExternalScreenSource`, Cemu over
  `src/core/external_frame_link.*`, which the Cemu fork shares byte for byte).
  The compositor and the layout system never learn which.
- **`InputSink`** (`src/input/ds_input.hpp`) takes button state and a touch
  point already in the console's coordinates, from this device's own input or
  from the other device's, over the network, through the same interface.
- **`OutputRouter`** (`src/render/compositor.hpp`) decides for each screen
  whether it is drawn in this window, handed to a system video layer (an
  iPhone's, iPad's or Apple TV's), or sent to the other device.
- The other device is a **companion** (`companion/`), the same session,
  protocol and menu code compiled for the Switch, Android, Apple devices and
  the desktop app's own Connect screens. See `src/net/` for the protocol.
- Every menu page is a **`MenuPage`** (`src/ui/menu_page.hpp`): it hears every tap in one place and
  says where it is scrolled in a measure that is the same at any screen size;
  the `lint_menu_pages` test fails for a page that reads its own pointers
  instead, and every screen has to say whether the other device shows it.

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

**Which of the two a session uses** is decided by the host, per screen, as it
goes — there is no setting for it:

- **A screen of 200,000 pixels or more goes as H.264** from its first frame: a
  Wii U GamePad, or a 3DS top screen at 2x render scale and up. At that size
  tiles cost many times what video does.
- **A smaller screen goes as tiles**, which are lossless and, at DS size, cost
  less than video does — until its tiles are measured at more than 2.5 MB/s
  (a busy 3D scene on a 3DS screen at a raised render scale). Then it goes to
  video, and stays there for 20 seconds before the tiles are given another try.
- **Tiles whenever video is not possible**: a build without FFmpeg, a companion
  that cannot decode it, or an encoder that just failed (tried again 30
  seconds later).

Each switch is logged (`the top screen goes as video now: ...`) with the
reason, and the first frame after one is a whole picture, so the two ends
never disagree about what is on screen. `--video-codec tiles|h264` forces one
or the other for a run, for comparing the two on your own machines; forced to
H.264 with no way to send it, the session says so on screen rather than
quietly sending tiles.

**When the wifi loses data.** Video is a chain: each picture is a
difference from the one before, so a picture that never arrives breaks every
picture after it until one that stands on its own (a keyframe) comes. A 720p
picture is dozens of datagrams, and losing any one of them used to lose the
picture: the screen held while a keyframe was asked for, and the keyframe --
dozens more datagrams -- could lose one just the same. That was a picture
frozen for seconds at a time. Three things stand in front of that now:

- **Recovery pieces.** Every video picture carries about a fifth again as
  many datagrams of Reed-Solomon recovery data (never fewer than two) -- the
  scheme Moonlight and Sunshine use. Any of them stands in for any missing
  datagram, so a picture comes through whole however its losses fall, up to
  as many as it has recovery pieces. Nothing to wait for; building them costs
  the host well under a millisecond for an ordinary picture.
- **Asking again.** A picture missing more than that is set aside and its
  missing datagrams -- or, for a small picture lost outright, the whole of
  it -- are asked for by name, the moment its last recovery piece has
  arrived and nothing more of it can be coming. On a home network the answer
  takes a few milliseconds. The pictures behind it wait and are shown after
  it, in order. How long an answer is waited for before asking again is
  learned from how long answers have actually been taking, the way TCP times
  its resends (between 8 and 30ms); after three unanswered askings, or 90ms,
  a keyframe is asked for instead. A keyframe's own missing pieces are asked for up to
  eight times, or 300ms, since giving up on one leaves nothing to show until
  another arrives whole. A copy of a piece that arrives after its
  picture is finished with -- both askings answered -- is recognised and
  dropped, and a picture is never shown behind a newer one.
- **A smaller picture.** A link that keeps losing pictures anyway -- two or
  more in a second, two seconds running -- gets a picture 20% smaller, and
  after ten calm seconds a larger one again. A larger picture that runs into
  trouble within 30 seconds doubles the calm it takes next time, up to 80
  seconds. Pictures recovered by asking again do not count as lost. The size rather than the bitrate, because the Deck's
  hardware encoder does not honour the bitrate it is asked for; fewer pixels
  is fewer bytes either way.

The companion reads the network on a thread of its own, so all of this
happens as datagrams land rather than once a refresh when the app gets round
to it: a missing piece is asked for, and the answer put in place, while the
app is waiting for the display. `DUALSCREEN_RECEIVE_THREAD=0` reads in the
app's loop instead, as before.

Both ends log what happened each second at the level a console shows:
`latency: ...` on the companion (pictures shown, frames lost, pieces rebuilt,
frames repaired and how long the answers took, pictures passed over waiting
for a keyframe) and `sending:
...` on the host (pictures sent, KB/s against the video allowance, round
trip, how long the send queue held things, keyframes and pieces asked for).

If a companion still cannot keep up, in order of how much they buy:

1. **Use the Separate Screens layout.** One screen per device rather than
   both on each: half the bandwidth, immediately.
2. **Get it onto a wire.** A USB Ethernet adapter in a Switch's dock is 100
   Mbit/s with none of the jitter, and needs nothing from this app — it is the
   same TCP either way. Docked Wi-Fi is usually better than handheld too.
3. **Turn the render scale down** on a 3DS game. A screen streamed at 3x is
   nine times the pixels of one at 1x.

## Latency

A tap on the companion's screen has to cross the network, be run by the game,
and come back as a picture before the player sees it land. A game that asks
for a moving target to be tapped, like the Goombas running across the screen in
Mario & Luigi: Bowser's Inside Story, stops being playable once that round trip
is too long. What keeps it short:

- **A frame leaves the host the moment it is made**, not on the host's next
  pass round its loop, which used to add up to a whole frame.
- **Video is encoded on a thread of its own**, the way Sunshine does it. The
  game's thread copies the picture (half a millisecond at 720p) and carries
  on; the encoder thread encodes it with nothing queued ahead of it and sends
  it the moment it is done. Encoding on the game's thread cost it 11ms a 720p
  frame with a software encoder, and on a Deck meant running VAAPI a frame
  deep -- a whole frame of latency on every picture -- so that a busy GPU's
  17ms waits did not stall the game. `DUALSCREEN_VAAPI_ASYNC_DEPTH=2` puts
  the old depth back, for comparing.
- **The companion draws each frame just before its display refreshes**, not
  straight after the last refresh, so the newest picture is the one shown --
  up to a refresh sooner. It measures the display's rhythm and its own work
  and backs off after a missed refresh; `DUALSCREEN_FRAME_DELAY=0` turns it
  off.
- **On an iPhone, iPad or Apple TV the system shows the video**, not GL. The
  decoded picture goes straight to an `AVSampleBufferDisplayLayer` laid over
  the window where the screen belongs -- no texture, no shader, and none of
  Apple's GL, which is a layer over Metal and the part of that platform most
  prone to stalling. While the quick menu is open the picture goes back
  through GL, which has to see it to blur it; the same decoded picture serves
  either way, so switching costs nothing. If the layer fails it is left
  alone and GL draws everything, as before; `DUALSCREEN_VIDEO_LAYER=0` does
  the same on purpose.
- **The companion holds a frame only as long as the link's own unevenness
  needs.** On a calm link that is a couple of milliseconds, not the whole frame
  it used to insist on. The pacing clock also keeps the stream's exact rate:
  in whole milliseconds, 16.67ms rounded to 17, the clock ran 2% slow, and
  the picture drifted up to four frames behind before catching up.
- **A backlog is let go rather than carried.** A companion drawing at the
  game's own rate shows one frame per refresh whatever happens, so the extra
  frame a Wi-Fi hiccup leaves queued would otherwise stay queued for the rest
  of the session. It is caught up on by letting one frame go unseen. How
  promptly depends on **Streaming Quality** in Settings:
  Responsive does it at once, Balanced (the default) after two frames, and
  Smooth after six.
- **A tap quicker than a frame still counts.** A press and release that both
  arrive between two of the host's frames used to reach the game as a release
  alone, which is a tap that never happened. The release now waits until the
  game has run one frame with the press.
- **The companion sends what was touched before it decodes what arrived**, and
  sends each change twice more on the next two passes, so one lost datagram
  does not lose a tap.

Measured on one machine, both ends drawing at 60Hz, from sending a tap to the
first picture made with it: over loopback, 33–84ms and drifting before this
work, and a steady 17ms after. Over a simulated Wi-Fi link (2–10ms per
datagram, and stalls of 15–35ms about twice a second) it is 33ms typical,
letting roughly one frame a second go unseen to stay there.

The companion measures this for real, too. The host tells it which picture
each input first reached, and with `--verbose` the companion logs a line like
`latency: input to picture 33ms typical, 33 to 50ms, over 15 inputs` about
once a second while inputs are changing. That covers everything this app
controls. It does not cover the display's own delay or the game's.

To reproduce those numbers on one machine, three switches exist for measuring
only. `--latency-probe` on the companion taps its own screen every 60ms.
`DUALSCREEN_TEST_VSYNC_HZ=60` makes either end wait for a refresh the way a
real display would, even under Xvfb. `DUALSCREEN_TEST_LINK=wifi` on the
companion delays what arrives the way the simulated link above does.


## Screen smoothing, and sending a screen

**Screen Smoothing** is a request rather than an instruction, and what happens to it depends on
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
renders on the GPU, so there is no copy of the frame in memory to send. At 1x
the screens stream at the console's own resolution — 400x240 and 320x240 — as
tiles. At 2x and 3x they are big enough to go as H.264 (see **Which of the two
a session uses**), and go at the render scale, so a companion gets the sharper
picture too; deflated RGB565 is a general-purpose byte codec whose cost grows
with every pixel a render scale multiplies, and at those sizes it made for a
choppy stream rather than a sharper one.

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

## Motion

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

## Layouts

The arrangements are measured off the Wii U's own output rather than eyeballed:
the sizes, the gaps and the alignments all come from its Screen Settings shots.
In the two focus layouts the small screen sits where that screen sits on a real
DS — the touch screen low, the upper screen high — so one pair is bottom-aligned
and the other top-aligned.

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
drawn. See [THIRD_PARTY.md](../THIRD_PARTY.md) for a note about where those images
came from.

Both are scaled to fit whatever display they are drawn on, the same picture
the Wii U Virtual Console's own Screen Settings shows at any size — filling
the screen rather than sitting pinned to the artwork's own pixel size on a
small display.

## Menu sounds

They are dry plastic ticks and pops, the closest to a modern console's menus,
and this project's own: synthesised from sine waves and noise by
`python3 assets/synth_ui_sounds.py` into `assets/ui_sounds/click/`, so they
carry nobody else's licence. `python3 assets/make_ui_sounds.py` trims them to
the sound, converts them to 48 kHz stereo and packs them into
`assets/ui_sounds.bin`, which is built into the app.

They play on an audio device of their own, opened at start-up, not with the
game's sound, which is queued a tenth of a second deep and paused while a menu
is up. On a Switch that is the console's plain audio output, beside the audio
renderer SDL plays the game through, fed by a thread that runs just above
the app's own so a busy frame cannot leave it with nothing to play; voices that
overlap are summed and then eased under full scale rather than clipped. Where SDL can open only one device at a
time -- Android, and a run with `SDL_AUDIODRIVER=dummy` -- there are no menu
sounds, since the game's sound needs that device; the log says which at

## Where things are kept

```
~/.config/dualscreen/settings.json          app-level defaults
~/.config/dualscreen/gamecontrollerdb.txt   optional controller mapping overrides
~/.config/dualscreen/games/<rom-sha1>.json  per-game layout
~/.local/share/dualscreen/saves/<rom>.srm   cartridge saves
~/.local/share/dualscreen/saves/<rom>.savestate1..4   save states
~/.local/share/dualscreen/saves/<rom>.srm.sync  which game a save is, and its versions
~/.local/share/dualscreen/saves/earlier/<rom>/  Earlier Saves, the newest 10
~/.local/share/dualscreen/saves/CTR-<title id>.srm.sync  a 3DS game's save record
~/.local/share/dualscreen/saves/WUP-<title id>.srm.sync  a Wii U game's save record
~/.local/share/dualscreen/saves/Azahar/sdmc/   Azahar's 3DS saves
~/.local/share/dualscreen/system/           where BIOS/firmware files are looked for
~/.local/share/dualscreen/dualscreen.log    this run's log
```

On Windows, what is under `~/.config/dualscreen` above is in
`%APPDATA%\dualscreen`, and what is under `~/.local/share/dualscreen` (saves,
cores, Cemu and the log) is in `%LOCALAPPDATA%\dualscreen`.

### settings.json

```json
{
  "last_browse_dir": "/home/deck/Emulation/roms/nds",
  "system_dir": "/home/deck/.local/share/dualscreen/system",
  "core_path": "",
  "core_path_3ds": "",
  "default_bilinear_filtering": false,
  "default_render_scale": 1,
  "audio_target": "both",
  "cursor_mode": "shown",
  "left_handed_layouts": ["upper_focus"],
  "stick_deadzone": 0.35,
  "trigger_deadzone": 0.3,
  "vsync": true,
  "wii_u_alone": "auto",
  "ui_sounds": true
}
```

`left_handed_layouts` names the layouts flipped for a left hand with the
stylus button under the layouts; every other layout is drawn right-handed. An
older file's single `"handedness": "left"` is turned into every layout that has
a hand, once.

`vsync` (**Vsync** in Settings) is off unless turned on. Off, a game played
here runs at its own rate -- a DS's is 59.83 frames a second -- and each
frame goes to the display as soon as it is drawn, which keeps input as quick
as it can be, above all a companion's. But a display refreshes 60 times a
second, so every few seconds a refresh has no new frame and shows the last
one again, and frames finishing near a refresh land on either side of it: a
small hitch every so often, however fast the game is running. On, and while
no other device is connected, a game whose rate is that close to the
display's runs one frame for each refresh, the way melonDS's own Switch app
does -- 0.3% fast for a DS game, which the sound follows without being heard
-- so every refresh shows a new frame. On a Switch that does not cost a
refresh of input either: the window has three buffers, which would let two
finished frames wait to be shown, so each frame is started by the clock
rather than whenever a buffer is free -- the display's period measured from
the first couple of seconds, a hair faster -- and when one does find another
still waiting, the next refresh is left to it. If the display stops keeping
up -- a frame held a tenth of a second, or several finding the queue full in
one second -- vsync is held off for ten seconds (twice as long each time it
happens again soon after, up to two minutes), so that the picture suffers
rather than the game. The game log's `speed:` lines count frames shown late,
how long drawing a frame took, and, locked to the display, refreshes left to
a waiting frame and frames that waited for a buffer.

`wii_u_alone` is `"auto"`, `"tv"` or `"gamepad"`, and starts as `"auto"`.
See [Wii U games](guide.md#wii-u-games). It replaces `wii_u_screen_alone`,
which is no longer read. Paths (`core_path`, `core_path_3ds`, `wii_u_program`,
`wii_u_mlc_path`, `system_dir`) are edited here rather than in the app. The BIOS folder
is the default one above unless `system_dir` names another, and the Settings screen
reports what it finds in it.

## The command line

```
dualscreen --game <path>     start a game straight away, in its last layout
dualscreen                   open the home screen
dualscreen --connect bottom  join the first game found as its bottom screen
                             (or top), skipping the menus
```

`dualscreen --help` lists every option. Besides those above: `--core` (a
libretro core for this run), `--layout` (a layout id for this run; see
`--list-layouts`), `--system-dir`, `--windowed`, `--verbose`, `--service` (the
Remote Start service), `--external-socket` (where a Wii U emulator hands its
screens over), `--video-codec tiles|h264` and `--trace-core-gl`.

Some options exist only for tests and measuring: `--exit-after-seconds`,
`--exit-after-frames`, `--screenshot <file>` with `--screenshot-at-seconds`,
`--simulate-key`, `--simulate-touch`, `--start-screen` and `--latency-probe`.
`tools/readme-screenshots.sh` uses them to render the pictures in the README.

## Tests

```sh
ctest --test-dir build --output-on-failure
```

The full suite takes about six minutes. `tests/CMakeLists.txt` lists every
test; a few of the main ones:

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
  of what the core accepted, the cartridge-save round trip, and a save state
  that puts the game back where it was when loaded. Also that run-ahead shows
  a touch in the frame it was read on a game that lags a frame
  (`MOCK_CORE_GAME_LAG`), is heard only once, and leaves the game exactly one
  frame on. Also checks
  that a screen read back for streaming is never sharper than what was
  actually rendered and never softer than the console's own resolution,
  whatever resolution is asked for.
- `test_quick_settings` — taps a grid over the quick menu and counts which
  taps do what, so the render-scale option is checked by being *found* rather
  than by being told where it is: present on a 3DS game, absent on a DS game,
  and every other option still reporting its own intent and no one else's.
  Also the list's order, moving across to its right-hand side and back, and
  the save state confirmations.
- `test_save_points` — save state files round-trip (state, picture and time),
  a truncated or foreign file is refused rather than handed to the core, and
  the picture is both screens one above the other whichever byte order they
  came in.
  `test_quick_settings` covers the menu side: the list's order, moving across
  and back, and asking before loading or overwriting and only then.
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
  wrong, before it has said), and that either end can end the session. Also
  the latency pieces: the pacing clock keeping a 60fps stream's exact rate,
  the hold's floor, how promptly each setting lets a standing backlog go, and
  a tap quicker than a frame still reaching the game.
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

Cheats, rewind and shaders. More than one companion per host. A Switch 2
that plays DS games itself. See [research-notes.md](research-notes.md) for
what was verified against the real melonDS DS core, and
[cemu-integration.md](cemu-integration.md) for the Wii U side.

The Switch, Switch 2, Android and Windows builds are built by CI on every push
(see [building.md](building.md)), and the iOS build is checked by
`.github/workflows/ios.yml`, but none of them can be run in CI on the real
hardware, so how they behave on a device is checked by hand.
