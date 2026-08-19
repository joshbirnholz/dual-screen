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
- **The menu chord.** Steam grabs the Guide button in Game Mode, so it is not
  used. L3, Select+Start, and a two-finger tap all open the menu.
- **Suspend/resume.** SteamOS sleep shows up as a very long gap between frames
  rather than as an event. Any gap over a second restarts the frame clock and
  flushes the audio queue instead of trying to catch up thousands of frames or
  playing out audio queued before the machine slept.

**What to check first on real hardware**, in this order:

1. That a blank fullscreen window appears at all as a non-Steam shortcut in
   Game Mode (the M0 gate the spec asked for).
2. Which GL context gamescope actually hands over — the log line reports it.
3. Whether the Steam Input layer presents the Deck's built-in controls as an
   SDL GameController, and whether L3 reaches the app or is consumed.
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
into the game.

**Why the font is embedded.** The app has to be able to say "your ROM is
missing" on a machine where nothing else is installed, and the Switch homebrew
target has no system font to borrow. It is 5x7 glyph art in
`src/render/font.cpp`, stored as readable ASCII rather than packed hex so a
typo is visible on inspection.

**Why audio is bounded rather than resampled.** The DS runs at ~59.83 Hz
against a 60 Hz display, so the queue drifts even when everything is behaving.
The queue is capped at about a quarter second and trimmed when it runs long,
which is inaudible and keeps input-to-sound latency from creeping up over a
long session. If the device refuses the core's sample rate, an `SDL_AudioStream`
converts instead.
