# Research notes

Answers to the open questions in the V1 spec, plus what is still unverified.
The spec said not to guess at these — this is what was actually checked, and
how, so the next person can re-check it when the core moves on.

## 1. Which melonDS DS core-option layout gives clean sub-rects?

**Answer: `melonds_screen_layout1 = "top-bottom"`, with
`melonds_number_of_screen_layouts = "1"` and `melonds_screen_gap = "0"`.**

Verified against the melonDS DS core's own option definitions
(`src/libretro/config/constants.hpp` and
`src/libretro/config/definitions/screen.hpp` in the melonDS DS source):

- The core does not expose one "screen layout" option. It exposes
  `melonds_number_of_screen_layouts` plus `melonds_screen_layout1` through
  `melonds_screen_layout8`, and cycles through the first *N* at runtime.
  Setting the count to `1` is what actually pins the layout — setting
  `melonds_screen_layout1` alone would still leave the player's cycle button
  able to move off it.
- `melonds_screen_layout1` accepts `top-bottom`, `bottom-top`, `left-right`,
  `right-left`, `top`, `bottom`, the four hybrid variants, and the three
  rotations. `top-bottom` is the one that yields two axis-aligned,
  equally-sized halves of a single buffer.
- `melonds_screen_gap` inserts dead rows *between* the two screens in exactly
  the layouts we care about, so it has to be `0` or the split coordinates move.
- `melonds_render_mode = "software"` keeps the core rendering into a CPU
  framebuffer, which is what V1's upload path consumes.
- `melonds_touch_mode = "touch"` makes the core read touch from the libretro
  pointer device, which is what the frontend feeds it.

Resulting geometry: **256x384**, top screen at `(0, 0, 256, 192)`, bottom
screen at `(0, 192, 256, 192)`.

**How this is enforced, rather than assumed.** Two things guard it at runtime:

1. `LibretroCore` records every option the core declares and validates each
   forced option against them. A key the core does not have, or a value it does
   not offer, becomes a warning shown to the player on the error screen
   (`LibretroCore::option_warnings`) — the app never silently renders a
   mis-split picture because a core version changed its options.
2. `compute_top_bottom_split` derives the crop from the *frame's own*
   dimensions each frame rather than from a hardcoded 256x384, requiring a 2:3
   width:height ratio and an even height. If the core ever produces something
   else (a gap crept back in, an upscaled OpenGL buffer, a different layout),
   the split is rejected with a readable reason instead of cropping the wrong
   pixels. Because it is derived, an upscaled 512x768 buffer splits correctly
   with no code change.

Both are covered by tests (`test_core_host`, `test_layout`).

## 2. Does gamescope's Game Mode have quirks with dlopen'd cores, GL context creation, or controller hot-plugging?

**Not yet verified on hardware — this needs a Steam Deck or a SteamOS machine,
and could not be tested in this environment.** The spec flagged this as the
riskiest assumption in the design, and it remains the one open risk. What has
been done is to remove every avoidable reason for it to go wrong:

- **Window creation.** The app asks for `SDL_WINDOW_FULLSCREEN_DESKTOP`, never
  a display-mode change, which is what gamescope expects from a well-behaved
  client. `--windowed` exists for desktop-mode development.
- **GL context.** An OpenGL ES 2.0 context is tried first and desktop GL 2.1 is
  the fallback; whichever is obtained, every GL entry point is resolved through
  `SDL_GL_GetProcAddress` rather than linked, and the renderer stays inside the
  ES2 feature set (no VAOs, no `GL_BGRA` upload, no `GL_UNPACK_ROW_LENGTH`).
  That is the smallest surface gamescope's nested compositor could object to,
  and it is also what the Android/Switch roadmap needs.
- **Controller hot-plugging.** Controllers are opened and closed from
  `SDL_CONTROLLERDEVICEADDED`/`REMOVED` at any time, not enumerated once at
  startup. Docking a Deck to a TV mid-session and plugging in a pad is the
  normal case, not an edge case.
- **The menu button.** Steam grabs the Guide button in Game Mode, and every
  other pad button is one a DS game might want, so no controller button opens
  the menu. F1 and a two-finger tap do; Steam Input can send F1 from any spare
  input.
- **Suspend/resume.** SteamOS sleep shows up as a very long gap between frames
  rather than as an event. Any gap over a second restarts the frame clock and
  flushes the audio queue instead of trying to catch up thousands of frames or
  playing out audio queued before the machine slept.

**What to check first on real hardware**, in this order:

1. That a blank fullscreen window appears at all as a non-Steam shortcut in
   Game Mode (the M0 gate the spec asked for).
2. Which GL context gamescope actually hands over — the log line reports it.
3. Whether the Steam Input layer presents the Deck's built-in controls as an
   SDL GameController, and whether a keyboard binding for F1 reaches the app.
4. Whether the Deck's touchscreen produces `SDL_FINGER*` events under
   gamescope, and whether they are normalized to the window as SDL documents.
5. Suspend and resume mid-game; confirm audio recovers and the GL context
   survives.

Items 3 and 4 are the ones most likely to need work.

## 3. BIOS/firmware requirements and redistribution

**No Nintendo-owned data is bundled, downloaded, or referenced by this
project.** The app locates user-supplied files and reports what it finds.

melonDS DS ships an open-source BIOS replacement and can boot DS-mode games
without any native files, so V1 treats all of them as optional and says so on
the Settings screen. Native files are needed for booting to the DS firmware
menu and for anything DSi:

| file | needed for |
| --- | --- |
| `bios7.bin`, `bios9.bin` | native DS boot |
| `firmware.bin` | booting to the DS menu |
| `dsi_bios7.bin`, `dsi_bios9.bin`, `dsi_firmware.bin`, `dsi_nand.bin` | DSi mode |

The Settings screen lists each with found/missing status against the currently
selected system folder, so "why won't it boot" is answerable without reading a
log.

## 4. Decisions made along the way

**Why the pointer rect is pushed into the core rather than assumed.** libretro
pointer coordinates span the core's whole output framebuffer, not one screen.
Since the core is pinned to top-bottom, a DS touch at `(x, y)` has to become a
pointer coordinate in the lower half of the buffer. That rect is computed from
the same `ScreenSplit` the crop uses and handed to `LibretroCore`, so the two
can never drift apart. The smoke test verifies the whole round trip: a touch at
a given point in the window makes the mock core draw its crosshair at exactly
that point.

**Why emulation pauses while the menu is open.** Comparing layouts is much
easier against a still frame, and it removes any chance of a menu press leaking
into the game *while* it is open.

**Why there is a held-input gate.** Pausing is not enough on its own. Button
state is polled, not edge-triggered, so the A press that chooses "Resume" is
still physically down on the frame after the menu closes, and the game reads it
as a fresh press. `HeldInputGate` records what is held at the moment the game
starts listening and suppresses exactly those inputs until they are released;
anything pressed afterwards passes straight through. The same applies to a
finger still on the screen. It is a separate class rather than a flag inside
the SDL polling so the behaviour can be tested directly, which it is.

**Why screen swapping is a layout, not a flag.** V1 originally had both: four
layouts plus a global swap toggle. That is two controls for one decision, and
the combinations are redundant (vertical + swap is vertical-swapped). The
swapped arrangements are now just more entries in the layout registry, which is
also the shape the registry was designed for -- new arrangements are data.

**Why the font is embedded.** The app has to be able to say "your ROM is
missing" on a machine where nothing else is installed, and the Switch homebrew
target has no system font to borrow. It is 5x7 glyph art in
`src/render/font.cpp`, stored as readable ASCII rather than packed hex so a
typo is visible on inspection.

**Why the C++ runtime is linked statically and libGL is not linked at all.**
The realistic way this reaches a Deck is: build in a container, copy the binary
to the host. That makes the host's runtime libraries the compatibility surface.
Measured on the built binary, the direct dependencies are `libSDL2-2.0.so.0`,
`libm` and `libc` — nothing else. `libstdc++` is static (`DUALSCREEN_PORTABLE`),
and `libGL` drops out on its own because every GL entry point is resolved
through `SDL_GL_GetProcAddress` rather than linked, so there is no GL or EGL
version to match.

The remaining coupling is glibc, and it is directional: a binary built against
an older glibc runs against a newer one, never the reverse. So the SteamOS
build script uses a Debian 12 container rather than the Arch one that SteamOS's
own lineage would suggest — a rolling Arch image has a *newer* glibc than the
Deck and produces a binary that will not start. Worth knowing: building on
glibc 2.39 raises the floor to 2.38 purely because GCC 13 rewrites `strtol`
into `__isoc23_strtol`; nothing in this source needs anything that recent.

**Saves are the frontend's job, and were simply missing.** In libretro a
cartridge save lives in the buffer behind `retro_get_memory_data(
RETRO_MEMORY_SAVE_RAM)`; the core never touches the filesystem for it. The
frontend has to read the `.srm` into that buffer after `retro_load_game` and
write it back. The first version of this frontend did not resolve those two
symbols at all, so every game started fresh, silently -- nothing errored,
because nothing was being asked for. Verified against melonDS DS's own
implementation: it returns the buffer only once a cart is loaded, and returns
null while it is showing an error screen, so both are handled.

Writes are atomic (temp file, fsync, rename) and happen on change rather than on
a timer alone -- every few seconds, on focus loss, on opening the menu and on
quit -- because a handheld is powered off without warning and a save is the one
file here that must never be half-written.

**Why everything is Nintendo positional.** Menus and game now share one rule --
east is A, south is B -- and SDL is asked to report face buttons by *position*
rather than by printed label, which removes pad-type detection from the input
path entirely. The earlier design had menus follow printed letters and the game
follow positions, which is defensible in isolation and confusing in practice:
the button that confirmed a menu was not the button the game called A. Sharing
one rule costs nothing and removes a whole class of "which one is this?"
questions. Pad type is still read, but only to decide which letters the
on-screen prompts print.

**Why face buttons were three decisions, not one.** The first attempt at fixing
this collapsed them into a single `face_buttons` setting, which could not
express the thing that turned out to be wanted: menus correct, game swapped.
They are independent and are now modelled that way -- how SDL reports the
buttons (pinned to "by printed label"), what confirms in a menu (always the
button marked A), and where the game places them (always a DS's own positions).
The game mapping resolves through *physical position* rather than swapping
letters blindly, so it is correct on a Switch Pro pad -- where nothing should
move -- as well as on the Xbox-layout pad Steam Input presents. None of it is
configurable: Steam Input is a better place to remap a pad than a second set of
bindings here.

A footnote worth remembering: the first attempt shipped a `face_buttons`
setting, and `Settings::save()` wrote it into everyone's settings.json on every
save. The follow-up read that key "for compatibility", which meant an
auto-written artifact nobody had chosen silently overrode the new default. A
legacy key is only worth honouring if it could only have got there deliberately.

**Why no controller button opens the menu.** Steam claims Guide in Game Mode,
and every other button is one a DS game might want. The menu opens on F1, which
Steam Input can be told to send from any spare input the player has. A
two-finger tap remains the touch-only route in. The cost is that a plain pad
with no Steam Input layer has no menu button at all.

**Why the face-button convention is explicit.** The input layer originally
carried the comment "SDL names face buttons by physical position (A = south)".
That is wrong: `SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS` defaults to `"1"`,
so SDL reports face buttons by their *printed label*, and on a Nintendo-layout
pad `SDL_CONTROLLER_BUTTON_A` is the east button, not the south one. Code that
assumes either layout mis-maps half the pads in circulation, and the menus and
the DS mapping had drifted onto different assumptions. The convention is now
named, pushed into SDL explicitly rather than inherited from a default, and
pinned by tests -- including one that drives an SDL virtual game controller
through the whole input path, which is the level the bug actually lived at.

**Why duplicate menu actions are dropped.** Steam Input presents a virtual pad
while the underlying physical device usually stays visible to SDL as well, so a
single press arrives as two identical button-down events. Two Confirms in one
frame in the file browser stepped into a folder and immediately back out of it,
which reads as the button doing nothing at all. Discrete UI actions are now
deduplicated per frame, and the browser additionally performs at most one
navigation per update -- `activate()` replaces the row list underneath the
loop that called it, so acting twice was operating on a directory the player
never saw. Both halves are tested; the browser test fails in exactly the
reported way if the guard is removed.

**Why audio is bounded rather than resampled.** The DS runs at ~59.83 Hz
against a 60 Hz display, so the queue drifts even when everything is behaving.
The queue is capped at about a quarter second and trimmed when it runs long,
which is inaudible and keeps input-to-sound latency from creeping up over a
long session. If the device refuses the core's sample rate, an `SDL_AudioStream`
converts instead.
