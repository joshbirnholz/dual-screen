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

## 1b. Which Citra core-option layout gives clean sub-rects?

Not in the V1 spec — 3DS support came later, and this is the same question
asked of the other console.

**Answer: `citra_layout_option = "Default Top-Bottom Screen"`, with
`citra_swap_screen = "Top"`.**

The 3DS is not a bigger DS. Its two screens are different shapes — **400x240**
above and **320x240** below — so nothing about the DS's "two equal halves"
reasoning carries over.

Under Citra's default top-bottom layout the framebuffer is **400x480**: the
upper screen fills the full width at `(0, 0, 400, 240)`, and the narrower touch
screen is centred beneath it at `(40, 240, 320, 240)`. `citra_swap_screen` has
to be pinned as well, or the player's swap toggle moves the two screens and the
crop lands on the wrong one.

`compute_top_bottom_split` handles both consoles from the same code by deriving
the scale from `frame_width / max(top.width, bottom.width)` and centring each
screen horizontally within the frame. That is exactly the DS case when the two
widths are equal, so there is one rule rather than a per-console table, and an
upscaled buffer (Citra's internal resolution multiplier) splits correctly with
no change. It is covered by `test_screen_source_handles_a_3ds_core`, which runs
against a mock core shaped like Citra and checks the crop lands on the colours
the mock painted.

**Citra does not render on the CPU at all.** This is the part a reading of the
option definitions alone missed, and it cost a release: `retro_load_game` calls
`RETRO_ENVIRONMENT_SET_HW_RENDER` before anything else and returns false if the
frontend declines. This frontend used to decline every hardware context — it
answered `GET_PREFERRED_HW_RENDER` with `RETRO_HW_CONTEXT_NONE`, which is honest
for a CPU-composited frontend and fatal for Citra. The symptom is "Failed to set
HW renderer" and no game.

What Citra asks for, read out of `citra_libretro.cpp` rather than guessed:

| field | value |
| --- | --- |
| `context_type` | `RETRO_HW_CONTEXT_OPENGL_CORE` on desktop Linux builds |
| `version_major.minor` | 3.3 |
| `bottom_left_origin` | true |
| `cache_context` | false |

It also calls `SET_HW_SHARED_CONTEXT` first, and `SET_PIXEL_FORMAT` with
XRGB8888 before either.

So the frontend implements the hardware-render path: it owns a framebuffer
object, hands the core its name through `get_current_framebuffer` and the
driver's entry points through `get_proc_address`, calls `context_reset` once the
framebuffer exists, and binds that framebuffer around `retro_run`. A frame then
arrives as `RETRO_HW_FRAME_BUFFER_VALID` rather than as pixels, and the two
screens are cropped out of it with `glBlitFramebuffer` — which also does the
vertical flip `bottom_left_origin` implies, for free.

Three consequences worth knowing:

- **The window's context depends on which console is being played.** ES 2.0
  cannot service a core asking for desktop 3.3, so the context a system needs is
  part of its profile and the window is rebuilt when a game needs a different
  one. A context stronger than a system needs is left alone, so going from a 3DS
  game back to a DS one rebuilds nothing.
- **A core profile removed the GLSL the renderer is written in.** `#version 120`
  with `attribute`/`varying`/`gl_FragColor` is not accepted there, and there is
  no default vertex array object to draw with. The shaders get a preamble that
  defines the old spelling back, rather than a second copy of themselves, and
  the renderer creates one VAO.
- **A GPU-rendered screen is already in RGBA order.** Core output arrives as
  XRGB8888 and the shader swizzles it back; doing that to a texture the GPU
  filled would turn it inside out. The texture carries its own channel order and
  the draw picks the matching shader path, so no caller has to know where a
  screen came from.

**The options Citra actually needs pinned**, all read out of its source rather
than assumed:

| option | value | why |
| --- | --- | --- |
| `citra_layout_option` | `Default Top-Bottom Screen` | the arrangement the split expects |
| `citra_swap_screen` | `Top` | Citra's way of saying "not swapped" — it has no disabled/enabled here |
| `citra_touch_touchscreen` | `enabled` | **off by default**; with it off Citra ignores the libretro pointer entirely and the touch screen does nothing |
| `citra_render_touchscreen` | `disabled` | its own pointer would sit on top of the one this app draws |

The first two were wrong on the first attempt, in the same way and for the same
reason: the mock core declared value lists this app had *invented* rather than
copied, so forcing `citra_swap_screen = "disabled"` passed every test and was
rejected by the real core. The mock now declares Citra's own values, which turns
a wrong pinned value back into a test failure. That is the general lesson here —
a mock that agrees with the frontend's assumptions tests nothing.

`citra_touch_touchscreen` is the one that would have been hardest to find from a
bug report, because nothing fails: the core loads, the game runs, and touches
are silently dropped. Citra reads `RETRO_DEVICE_ID_POINTER_PRESSED` only inside
that option's branch; the default branch (`citra_mouse_touchscreen`, on by
default) takes its press from `RETRO_DEVICE_ID_MOUSE_LEFT`, which this frontend
never sends.

**Rendering at a higher internal resolution** is one more option in the same
table, `citra_resolution_factor`, and it is the one the player actually gets to
change: `1x (Native)|2x|3x|4x|5x|6x|7x|8x|9x|10x`, spelled exactly like that —
`"2"` and `"2.0"` are not values it has, and the first one is written out in
words. Citra parses everything before the `x`, so the frontend builds the string
the same way.

Changing it mid-game needed nothing new. Citra sees the change through
`GET_VARIABLE_UPDATE` at the top of its next `retro_run`, recomputes its layout,
and announces the result with a whole new `retro_system_av_info` — base *and*
maximum both set to the new size. The existing path already handled that: the
framebuffer grows on `SET_SYSTEM_AV_INFO`, the frame that comes back is measured
rather than assumed, and `compute_top_bottom_split` derives its scale from
`frame_width / max(top.width, bottom.width)`, which is exactly the "an upscaled
buffer splits correctly with no change" claim above being cashed in. The only
new code is the option string and the button.

What does *not* change is `screens_geometry()`, which keeps reporting the
console's own 400x240 and 320x240. Layout placement is computed from that and
not from the texture sizes, so every layout puts the screens in the same place
at any scale and only the sharpness differs — checked by running every shared
3DS layout at 1x and 2x in the smoke test and comparing where the screens
landed, and against the real core with a homebrew ROM, where the lit bounding
box came out identical to the pixel.

The menu stops at 3x. That is a judgement about the hardware, not a limit of the
core: past 3x a Deck pays for pixels nothing can display, and a cycle through
ten values is a worse control than a cycle through three.

**The circle pad.** A 3DS is played with an analogue stick, and this frontend
sent only digital buttons — so a 3DS game would have loaded, drawn and been
impossible to walk around in. `DsInputState` now carries the stick in libretro's
own range, `RETRO_DEVICE_ANALOG`'s left index answers with it, and it travels
over the wire so a companion device can drive it too. A DS core is asked for it
and ignores it, so there is nothing to switch on.

**A GPU core does not hand the context back as it found it.** This is the third
thing that only showed up against the real core, and the most costly: the game
ran, but its screens were blank and the menu's text came out as solid
rectangles. Citra ends every frame with its own `ResetGLState()`, which is not a
reset to anything a frontend would want:

| what it leaves | what it does to this app |
| --- | --- |
| `glBlendFunc(GL_ONE, GL_ZERO)` | source alpha ignored, so text drawn from a coverage mask fills every glyph cell solid |
| `glEnable(GL_DEPTH_TEST)` | harmless here only because the window has no depth buffer |
| a live scissor rect from its own rendering | clips both draws **and** framebuffer blits, which is what blanked the screens |
| bound program, textures and vertex array reset | a core profile draws nothing at all with no vertex array bound |

The frontend had set its blend mode and bound its vertex array once, in
`initialize()`. That is fine for a CPU-rendering core and worthless for a GPU
one: none of it survives the first frame. `gl::reset_render_state()` now
re-establishes the whole lot every frame from `QuadRenderer::begin_frame`, and
again before the screen blit, which runs before the frame proper begins. The
rule is that the renderer assumes nothing about state it did not set this frame.

The mock core now performs Citra's `ResetGLState` verbatim after presenting, so
this is a test failure rather than a bug report. That is the same lesson as the
option values, twice over: **a mock that is better behaved than the real core
tests nothing.** Each time this project has been surprised by Citra, the mock had
been written to match an assumption rather than the source.

**Size the core's framebuffer to what it draws, not to what it might.** Citra's
`retro_get_system_av_info` declares `max_width`/`max_height` as ten times its
base — room for a resolution multiplier it is not using — while
`base_width`/`base_height` stay 400x480. Sizing the framebuffer from the maximum
leaves the picture in one corner of a buffer a hundred times its area, and every
read out of it then depends on knowing *which* corner. libretro says the origin
is bottom-left when the core asks for that, but a mock written by the same
person who wrote the reader will always agree about where the picture went, so
the mock cannot settle it.

Sizing from the base removes the question: buffer and picture are the same
rectangle, and it is the same rectangle whichever convention the core used. It
also reclaims 76MB on a handheld. Growth is still handled — a core announces a
size change through `SET_GEOMETRY` or `SET_SYSTEM_AV_INFO` before drawing at the
new size, `run_frame` re-checks in case one ever draws larger unannounced, and
the placeholder allocated before any size is declared is replaced by the real
size rather than becoming a floor it cannot shrink below.

**Hand the core OpenGL's default state, not yours.** This was the black screen,
and it took a call trace on the real core to see it. Citra was reaching the
right framebuffer, setting the right viewport, binding its textures and issuing
its draws with no error — and the result was black.

The reason is how every emulator of any size drives OpenGL. State changes are
expensive, so the core keeps a *shadow* of the GL state and only issues the
calls where its belief differs from what it wants. That shadow starts at the GL
defaults. So anything the frontend leaves switched on is **invisible** to the
core: it will not turn it off, because as far as it knows it is already off.

This app enables `GL_BLEND` with `SRC_ALPHA, ONE_MINUS_SRC_ALPHA`, which its own
text and UI need. Citra draws its screens with no blending, so it never disables
blending, so its screens were composited against the background through their
own alpha — and 3DS screen data has no meaningful alpha. Every pixel it drew
resolved to the background. Drawn correctly, landing nowhere, no error anywhere.

`gl::reset_state_for_core()` now puts the whole fixed-function state back to the
GL defaults before every `retro_run`. The rule is: **the frontend's rendering
state is the frontend's business, and the core is entitled to start from the
defaults its own bookkeeping assumes.**

It is the mirror image of the earlier lesson about a core not restoring state
after itself. Both directions matter, and neither is optional: the frontend
must reset before handing over, and must not trust anything after getting it
back.

The mock now checks what it is handed at the top of `retro_run` and reports
anything non-default, so this is a test failure rather than a black screen.
Removing the reset produces exactly what the real core produced:
`frontend left non-default GL state: GL_BLEND`.

**Verified against the real core.** `tools/probe_core` loads a genuine libretro
core against a real OpenGL context and reports what it asks for. No ROM is
needed, which is the point: a core requests its hardware context, pixel format
and options before it looks at the game file. Run against the Citra build from a
Steam Deck's RetroArch:

```
core: Citra 4268124  extensions: 3ds|3dsx|cia|elf
SET_PIXEL_FORMAT: 1                    (XRGB8888)
SET_HW_SHARED_CONTEXT requested
SET_HW_RENDER: context_type=3 version=3.3 depth=0 stencil=0 bottom_left_origin=1 cache_context=0
```

Every value this frontend was built against, confirmed from the core itself
rather than from reading its source: `context_type=3` is
`RETRO_HW_CONTEXT_OPENGL_CORE`, at 3.3, bottom-left origin, no cached context.
And `depth=0, stencil=0` — so no depth or stencil attachment belongs on the
render target, and a depth test can never be what discards a frame here.

This is the tool that should have existed before any of the 3DS work. Every
wrong turn in this file came from a mock that agreed with an assumption instead
of with the core.

**The bug: a sampler object the core left bound.** This was the black screen,
and nothing short of running the real core found it.

Citra binds an OpenGL *sampler object* to texture unit 0 and does not unbind it.
A sampler object bound to a unit **overrides the filter and wrap settings of
whatever texture is bound there** — the texture's own parameters are ignored
entirely. Citra's sampler asks for a mipmapped minification filter. This app's
screen textures have no mipmaps, so under that sampler they are *incomplete*,
and sampling an incomplete texture yields solid black.

Everything else kept working, which is what made it so hard to see:

| what | why it still worked |
| --- | --- |
| solid quads (menus, panels, letterbox) | no texture, so no sampler involved |
| DS games | melonDS renders on the CPU and never binds a sampler |
| the core's own rendering | Citra's textures suit Citra's sampler |
| every GL error check | nothing here is an error; an incomplete texture is legal and samples black |

So the window was *perfectly* black — not dark, not wrong-coloured, but every
pixel exactly zero — while the picture sat correctly in the screen texture the
whole time. `gl::reset_render_state()` now calls `glBindSampler(0, 0)`.

**How it was found.** By working backwards along the picture's path with probes,
once the real core and a homebrew ROM made it reproducible locally:

1. The core's framebuffer had content (`brightest pixel 765`), so Citra rendered.
2. The screen texture had it too — a pixel-for-pixel match with the source
   (`source rgb(91,68,5) -> destination rgb(91,68,5)`), so the crop was fine.
3. The compositor was placing it (`rect 344,11 591x354 from texture 13, tint white`).
4. A solid red rectangle drawn at the same moment appeared; the textured draw
   next to it did not.

Step 4 is the one that named it. Solid drawing working while textured drawing
silently produced nothing points at exactly one thing.

**What is still unverified:** the rendering itself is now verified against the
real Citra core with a homebrew ROM, in all three shared layouts. What remains
untested here is a commercial game and real hardware. The earlier caveat that
this was exercised against a mock core that
requests the same context Citra does and renders through it, not against a real
Citra build, which was not available in this environment. The frontend's runtime
guards are what stand in: a forced option the core does not declare becomes a
visible warning, a hardware context this window cannot service is declined with
a reason rather than half-accepted, and a driver too old for 3.3 produces a
readable error screen instead of a failed start.

**3DS system files.** As with the DS: none ship here and none are downloaded.
Citra needs files that come off your own console, and the Settings screen's
inventory covers the DS files only, since Citra's requirements vary by fork.
What the Settings screen does report for 3DS is whether a core was found at
all.

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

**Reported symptom: the app quits instantly in Game Mode but works in desktop
mode.** Not reproducible here, so the response was to remove the reasons a
startup failure would be *invisible*, and to widen the paths most likely to be
failing:

- Every run writes a log to `~/.local/share/dualscreen/dualscreen.log`, opened
  before anything that can fail. Game Mode has no terminal, so previously a
  failed start and a crash looked identical from outside.
- `SDL_Init` no longer asks for the game controller subsystem as a hard
  requirement. It was bundled with video in a single call, so a controller
  subsystem that fails -- plausible inside Game Mode's sandbox, where device
  permissions differ -- took the whole app down with it. It is now initialised
  separately and a failure is a warning.
- The GL context fallback chain went from two attempts to five, ending with one
  that requests no profile at all and takes the driver's default. An explicitly
  profiled request can be refused where the default succeeds, and any working
  context beats refusing to start. Each attempt is logged with its reason.
- Error screens cannot be dismissed for the first three quarters of a second.
  A launcher that delivers a button press as the app starts could otherwise
  dismiss a fatal error before it had been drawn once -- which from outside is
  indistinguishable from quitting for no reason.

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
Since the core is pinned to top-bottom, a touch at `(x, y)` has to become a
pointer coordinate in the lower part of the buffer. That rect is computed from
the same `ScreenSplit` the crop uses and handed to `LibretroCore`, so the two
can never drift apart. The touch screen's *own* size travels with the rect in
the same struct, because converting a touch needs both and taking one from one
console and one from another misplaces every press by exactly the ratio between
them — which is what a 3DS touch did while `pointer_axis` still divided by the
DS's 256x192. The smoke test verifies the whole round trip on both consoles: a
touch at a given point in the window makes the mock core draw its crosshair at
exactly that point.

**Why a touch that misses the screen clamps to its edge instead of being
dropped.** `map_output_point_to_ds` used to require the point to land inside the
bottom screen's own pixel rect, and reject it otherwise. That is stricter than
a real touch screen: the small screen of a focus layout, the bezel plastic
around `standard`, the gap between two stacked screens in `vertical` are all
places a finger legitimately lands a few pixels short, and a real stylus
dragged past the glass stops at the edge rather than registering nothing. The
fix computes `u, v` the same way as before but clamps each to `[0, 1]` before
the rotation switch rather than requiring containment first. Clamping before
the switch rather than after works out to the same point either way -- the
rotation cases are just a relabelling and reflection of `u` and `v`, so
`clamp(1 - u)` and `1 - clamp(u)` agree at every value -- and it is the point
this is a fix for, so it is where the reasoning is worth spelling out rather
than trusted by inspection.

The one place this must *not* apply is a tap that landed on another screen
instead: the upper one, on every layout that shows both. That is not a miss, it
is a look at a different picture, so the touch screen still ignores it
entirely. The function now finds the bottom placement, then checks the tap
against every *other* placed screen and only clamps if none of them claim it --
generic across DS and 3DS and every layout, since it works from `place_screens`
rather than anything system-specific. Covered by the round-trip test's existing
corner cases, extended to the pillarbox beside the touch screen and a point
past two edges at once, both landing on the near point or corner rather than
being lost.

**Why the console picture is per system and where its numbers come from.** The
Standard layout is the one that draws a console, and a 3DS is not a DS with
different screens: different body, different proportions, different holes. So
`BezelArt` holds one texture per system and the app hands the compositor the one
for whatever is being played -- which on a companion means whatever the host
says is being played, since it follows the host's console too.

The screen rectangles are measured off the artwork rather than derived from the
real machine's millimetres, which is what the 3DS profile used to do while it had
no picture. Both images come with an overlay marking where the screens go, and
reading that overlay gives whole pixels: on the 3DS, 400x240 at (131, 66) and
320x240 at (171, 391) in a 663x688 image -- one image pixel per 3DS pixel, the
same relationship the DS artwork has. That is what lets both consoles be drawn at
the artwork's own size on a 1280x800 display and have every screen land exactly
one-to-one.

The smoke test checks the two agree by probing pairs of points: one well inside
each screen edge, which must be the game, and one just outside it, which must be
console plastic. A rectangle measured wrong still produces a plausible-looking
picture, and only a pair like that notices.

**Why the console is a profile rather than a set of constants.** Screen sizes,
ROM extensions, core filenames, the console picture and the core options to pin
were all DS constants scattered across the layout code, the screen source, the
protocol and the browser. They are now one `SystemProfile` per console in
`src/core/system.cpp`, and everything that used to know 256x192 takes a
`SystemScreens` instead. The geometry parameters are deliberately *not*
defaulted: a caller that forgets to pass one fails to compile rather than
quietly getting a DS's. Adding a console is a profile plus its option pinning,
not a hunt for constants.

**Why the console comes from the ROM's extension.** Nothing else about a
session establishes it early enough. The extension picks the profile, the
profile picks the core, and the choice rides the wire in the welcome message so
the companion adopts the host's console rather than guessing from the frame
size it happens to receive.

**Why the render scale is shared like everything else on that menu.** It is
tempting to treat it as the host's private business, since the host is the only
device running a core. That is the wrong seam: the in-game menu is one menu that
happens to be reachable from either device, and a player holding the companion
should not have to walk over to the other machine to change how the game looks.
So it travels the same road as smoothing, handedness and the sound target — the
companion asks, the host applies it to the core, and the result comes back as
session state. The rule the footer actually follows is that a button appears
where there is something to choose, not where the machinery happens to live: a
DS game has no button, because melonDS is pinned to its software renderer and
there is no multiplier to set.

**Serving a screen from a core that renders on the GPU.** The host used to hand
the network layer a pointer into the core's framebuffer and let it read the two
screens out. That is exactly right for a software-rendered core and meaningless
for Citra, which never puts a frame in memory at all: `frame.pixels` is null, so
the arithmetic ran off a null base. The top screen sits at (0, 0), so its
pointer stayed null and the guard against a null pointer skipped it -- a
companion connected, heard audio, and saw a placeholder forever. The bottom
screen sits further in, so its pointer was null plus an offset: not null, not
mapped, and the host segfaulted the moment anything needed that screen. Which is
every layout except the split one, and the split one too as soon as the
companion opened its menu, since a menu draws every layout in miniature and
needs both.

So the frontend asks the *source* for a screen rather than reaching into a frame
it does not own: `read_screen()` hands back a view into the core's own
framebuffer where there is one, and reads back off the card where there is not.
Three things fall out of that:

- **Only what is being sent is read.** A read back off the GPU stalls the
  pipeline, so `companion_wants()` is public and the caller skips the screen
  nobody is going to see.
- **At whatever resolution the recipient actually needs, not always the
  console's.** The crop into the staging texture scales, so this is a hint the
  caller can act on. See below for what decides the hint.
- **RGBA in, XRGB out.** `glReadPixels` is asked for `GL_RGBA` rather than
  `GL_BGRA` -- every driver takes that one -- and the swizzle costs a fraction
  of what compressing the frame costs. Getting it wrong turns the upper screen
  red and leaves the green one below looking perfectly fine, which is why the
  two-device test that checks the colour is the one where the companion takes
  the *top* screen.

No flip is involved: `glReadPixels` starts at the framebuffer's bottom-left,
which for these textures is the screen's top row, because the crop that filled
them already turned the core's bottom-left origin into the top-row-first order
everything here is uploaded in. The touch round-trip in the two-device test is
what proves it -- a flipped picture would put the crosshair at 0.75 instead of
0.25.

**Every screen used to be streamed at the console's own resolution, whatever
the render scale was set to** -- reported as a bug once render scale existed
to make it visible: turning it up sharpened the picture on the device running
the core and did nothing at all for a companion, which is backwards for two
devices meant to be looking at real screens. The fix has to answer two
questions the render scale alone cannot: how much *can* be sent (bounded by
what was actually rendered -- there is nothing above that to send), and how
much is actually *worth* sending (bounded by what the recipient can do with
it, which nothing on the host side knew until now).

The second bound needed a fact the protocol had never carried: the
companion's own window size. It travels as one more field on `RequestMessage`
-- `wants_display_size`/`display_width`/`display_height` -- sent once on
connecting and again on every resize, protocol version 7. It does not belong
in `SessionStateMessage` alongside layout and smoothing: those are choices the
host owns and broadcasts back so the two devices cannot disagree, but a
display size is not a choice at all, just a fact one device is reporting about
itself, and broadcasting it back to the companion that already knows its own
window size would be pointless. `HostSession` keeps it privately -- a getter,
nothing more -- the same way it already keeps `companion_menu_open_` outside
the request/adopt machinery built for the shared settings.

Turning a window size into a target resolution reuses machinery that already
existed for a different question. `device_layout()` answers "what does *this*
device draw", resolving handedness and, for a per-device layout, which screen
this device holds; `App::Impl::companion_target_size()` is the same question
asked about the companion instead -- `resolve_for_device()` already took a
screen ID rather than "the local one", so nothing there had to change -- and
then hands the result to `place_screens()`, the function the compositor, the
menu previews and touch mapping already treat as the single source of truth
for where a screen lands. The box that function says a screen fills in the
companion's window is what its resolution should match. One piece of this
needs to be undone: `place_screens()` reports that box already turned for a
rotated placement (`side_by_side` draws both screens a quarter turn), but the
picture sent over the wire is always in the screen's own unrotated axes --
rotation is applied by whichever device draws it, not baked into what is
sent -- so a quarter-turned box has its width and height swapped back before
it becomes the target.

`LibretroCoreScreenSource::read_screen()` treats that target as a hint, not an
instruction: `want = clamp(hint or native, native, rendered)`. Below native,
clamped up -- a picture duller than the console's own resolution stops being
worth looking at, and every other part of this app already treats native as
the floor. Above what was rendered, clamped down -- there is nothing sharper
to send, and sending it anyway would cost bandwidth for detail that does not
exist. A software-rendered core (melonDS, still) has nothing above native to
offer in the first place, so the hint has nowhere to act and the CPU
zero-copy path is untouched. Verified against the real Citra core with two
live processes at once: a `vertical` companion at its full 1280x800 window
asked for less than 2x rendered and got exactly that (592x355, not 800x480);
an `upper_on_tv` companion at 3x asked for more than was rendered and was
capped to exactly what 3x produced (960x720, not the 1066x800 its window
geometry alone would have wanted). One log line per screen, only when the
target size actually changes, names which case fired.

**What this does not do.** The wire format is still deflated RGB565 -- the
same encoder as before, just handed a differently-sized frame. That is fine
for what a DS or a 3DS actually produces: mostly flat 2D content that deflate
compresses well, at a resolution low enough that even an incompressible frame
is cheap. It stops being fine long before Wii U resolutions: full-motion 3D
content deflates poorly, and a general-purpose byte compressor recompressing
an entire frame from scratch every tick, with no motion compensation and no
reference to the frame before it, is not what real-time video at those sizes
needs. Adaptive resolution is the right first move regardless -- it is
wasteful to send more than the recipient can use whatever the codec -- but it
is a bound on *how much* is sent, not a different way of encoding it, and a
real video pipeline (hardware encode on the host, hardware decode on
whichever device is receiving) is a separate, larger piece of work that this
did not attempt.

**Why the send queue's backlog limit is measured in frame-times now, not a
fixed byte count.** `kMaxQueuedBytes` was 512 KiB, sized for a DS's picture.
Once a screen can legitimately be sent at several times that -- a 3DS at 2x
with real content deflates far short of the ~4x pixel-count savings 2D content
gets -- the same constant either starves a companion that could actually keep
up, or is too small to ever hold one whole frame at all, which silently drops
*every* frame rather than the occasional one a backlog limit exists to catch.
`HostSession::queue_limit()` is `max(kMinQueuedBytes, last frame's payload x
3)` instead: it rises and falls with whatever is actually being streamed,
`kMinQueuedBytes` is only the floor for before a size is known, and the
bound that matters -- how many frames of staleness a companion is allowed to
fall behind by -- is now a property of frames rather than of bytes, which is
what it was always meant to mean.

**Streaming above native resolution was much slower than 1x, and the cost was
self-inflicted.** A GPU-rendered screen's readback used to spend a full pass
over every pixel swizzling `glReadPixels`'s R,G,B,A order into the XRGB8888
`encode_screen_pixels()` expects, right before that same encoder spent a
second full pass over the same pixels packing them down to RGB565. Local
rendering never pays this at all -- `Texture::ChannelOrder` lets the shader
read a texture in whatever order it actually arrived in, so `upload_screens()`
uploads GPU output straight through with no CPU touching it. The network path
had quietly reinvented the CPU-side conversion the render path had already
learned to avoid, and at a render scale above 1x it is the same waste multiplied
by however many times more pixels that scale produces. `ScreenReadback` now
carries a `channel_order`, set to `Rgba` by the GPU path instead of swizzling;
`encode_screen_pixels()` reads whichever byte holds red and which holds blue
directly rather than assuming XRGB8888. One pass over the frame instead of two,
scaling with render resolution the same way the rest of the cost already does.

**A companion that vanished without saying goodbye could wedge the host's one
connection slot shut, permanently.** Sleep, a dead Wi-Fi link, or the app being
killed outright never sends `Bye`, and nothing was watching for that: sockets
here had no keepalive, so a stream with a gone peer looks exactly like an idle,
healthy one -- `recv()` just returns "try again" forever. `HostSession::poll()`
only calls `accept()` while its one companion slot is empty, so that phantom
connection sat in the slot indefinitely, and every real reconnect attempt after
it completed its own TCP handshake (the kernel accepts into the listen backlog
whether or not the app has called `accept()` yet) and then simply waited, since
the host never got around to noticing it was there. `TcpStream` now turns on
`SO_KEEPALIVE` with `TCP_KEEPIDLE`/`TCP_KEEPINTVL`/`TCP_KEEPCNT` shortened from
the OS default of two hours to about ten seconds, on both the accepting and the
connecting side, so a truly dead peer is noticed and dropped automatically
rather than requiring the host to be restarted.

**Adaptive streaming resolution is reverted to native-only, for now.** The
channel-order fix above halved the CPU cost of streaming a GPU-rendered
screen, but at 2x and 3x render scale that was still not enough for a smooth
picture: the RGB565-pack-and-deflate encoder is a general-purpose byte
codec, not a video one, and its cost is still proportional to the pixel
count a render scale multiplies. Rather than ship a choppy "sharper"
picture, `App::Impl` no longer asks `read_screen()` for a companion-sized
target at all -- every screen streams at the console's own resolution
regardless of render scale, which is what made the picture smooth in the
first place. The adaptive-resolution plumbing itself (`RequestMessage`'s
display-size fields, `HostSession::companion_display_size()`) is left in
place rather than torn out, since it is exactly what a real hardware video
pipeline -- encode on the GPU, decode on the GPU, an actual codec bitstream
instead of deflated RGB565 -- will need in order to be worth turning back on;
that is also what Wii U-scale full-motion content requires, and is future
work, not something this reverts.

**The Standard layout no longer locks to the console artwork's own pixel
size on a small display.** It used to: on a window no bigger than a Deck's
(1280x800), the console frame was pinned to its own pixel dimensions rather
than scaled up, so that every screen came out exactly one-to-one and never
needed resampling. That traded away too much -- on a Deck-sized window the
console ended up much smaller than the screen, nothing like the Wii U
Virtual Console's own Screen Settings picture, which fills the display at
any size. `layout_frame()` now always scales the console art to fit,
the same as it already did above that threshold; a screen landing exactly
on its own pixel size is still caught and left unsmoothed by the
general-purpose `mark_native()` mechanism (see `test_native_screens_are_
spotted_anywhere`), it just no longer happens by construction at one
specific window size. The now-unused `native_width`/`native_height`/
`native_max_output_*` fields and the `as_preview()` helper that stripped
them for preview tiles are gone with it -- a preview tile was always just
`place_screens()` given a smaller rect, and now that is all it needs.

The focus layouts' small screen was also re-measured larger, closer to what
the Wii U Virtual Console's own Screen Settings picture actually shows --
the previous measurement read a screenshot that made it look smaller than
it is.

**The left stick is the circle pad, and on a 3DS that is all it is.** The stick
used to set the d-pad bits unconditionally, on the reasoning that a DS has no
stick and a player will reach for one anyway. That is right for a DS and wrong
for a 3DS, which has both: every core that reads the d-pad saw a direction held
down for as long as the stick was off centre, so walking came out as the eight
directions the d-pad has and the circle pad could not be used. It is now a
property of the console -- `SystemProfile::has_analog_stick` -- rather than a
rule about controllers.

Fixing that turned up the larger half of the same bug: `poll_held_buttons()`
computed the circle pad into a local `DsInputState` and returned only the
buttons out of it. Every layer underneath already carried the stick -- the wire,
the analog callback, the core -- and each was tested; the one step that fills it
in from the physical pad was not, so a 3DS was always handed a centred circle
pad. Polling now returns the whole state. The lesson is the mirror of the mock
one: testing every layer of a chain separately does not test the chain.

The held-input gate learned about the stick at the same time. The menu is
navigated with it, so it is very often still pushed at the moment the game
starts listening again -- the same leak the gate was written for, one input
later.

**Why smoothing depends on the texture and not only on the setting.** "Off"
means "keep the original pixels square", and that is a statement about being
drawn bigger than life. It says nothing useful about the opposite case, which
arrived with the render scale: a core rendering at 2x into a rect the size of the
console's own screen hands the GPU twice the pixels there is room for, and
point-sampling that drops every other row -- the exact detail the higher
resolution was paid for. So the rule is: one texel per pixel is never resampled,
more texture than room always is, and everything in between follows the player.
`screen_needs_smoothing()` is the single place that decides it, because the
compositor and the menu's layout previews both draw screens and a rule living in
only one of them is a rule the other quietly disagrees with.

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

**Why the companion's screen choice was ignored.** The host decoded
`hello.wanted_screen` and then never read it, granting the opposite of its own
fixed screen instead -- so "connect as top" and "connect as bottom" did the same
thing. The host now takes whichever screen the companion did not ask for, and
advertises both as available until one is claimed, since it adapts either way.
Two-process coverage runs for both choices: a single-direction test would have
passed throughout the bug.

**Why audio carries a stream id when there is only one stream.** DS and 3DS have
one soundtrack, so both devices play the same thing and the id is always
`Shared`. Wii U genuinely does not -- the GamePad speaker is not a copy of the
TV's -- so the field exists now, with the per-device values reserved. A
companion ignores a stream that is not `Shared` rather than playing it on the
wrong device, which means adding Wii U later does not require a protocol
version bump for this.

**A test trap worth remembering.** Several session tests waited for
`host.has_companion()` and then immediately checked something the companion
learns from the welcome. That returns as soon as the *host* has processed the
hello, before the companion has read the reply, so the assertions saw unset
values. Waiting on one side's view of a handshake is not waiting for the
handshake; the tests now require both ends to agree it finished.

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

**Why smoothing is handled above every screen, not inside the emulator.**
melonDS DS has its own scaling options, but they only exist for the core's own
output -- they would say nothing about a screen arriving over the network. The
toggle instead sets the sampling mode on the two textures a `ScreenSource`
hands out, which is the one place both a locally emulated screen and a streamed
one pass through, so `set_filtering` is a method on that seam. It also means the
choice is per device: the Deck can smooth its half while the TV stays sharp,
which is the right answer when the two screens are physically very different
sizes.

**Why R3 is read before the screen it lands on.** The toggle means the same
thing everywhere, so the app consumes `ToggleFilter` before dispatching to
whichever screen is up. The first version also let the layout picker handle it,
which toggled twice and cancelled itself out -- the button did nothing at all
whenever the menu was open. The picker now ignores the action and is told the
result instead. What proved it is the end-to-end check: the smoke test renders
one frame each way and asserts the two pictures differ, which no unit test on
the stored flag could have seen.

**Why touch is mapped against what one device is showing.** The compositor drew
with the per-device layout while the touch mapping still used the shared one.
For every layout that shows both screens on both devices those are the same
object, so nothing was wrong until "one screen per device" arrived -- there the
companion fills its window with the bottom screen while the shared layout still
describes a stacked pair, so the upper half of the companion's screen mapped to
nothing at all and the lower half landed at roughly double the offset. Both now
come from `device_layout()`. The two-process test holds a touch on the
companion and checks the mock core's crosshair comes back at the same point,
which fails in exactly the reported way against the old code; the top-screen run
asserts the opposite, that a device holding no touchscreen sends nothing.

**Why the pointer auto-hides rather than being gone.** It was hidden outright,
which is right for a console but wrong for a trackpad: the bottom screen is a
touchscreen, and an arrow you cannot see is one you cannot aim. It now appears
on mouse movement and hides two seconds after the movement stops. Touches do not
count as movement -- SDL reports them as mouse events from `SDL_TOUCH_MOUSEID`,
and a tap that left an arrow on the screen would be the original problem back
again. The timing lives in `CursorVisibility` rather than in the event loop so
it can be tested without a mouse.

**Why the host owns every shared setting.** The layout was already host-owned --
the companion asked and adopted the answer -- but smoothing and the sound target
were not, so a companion could sit at a different setting from the host it was
showing. They are all session state now, carried in one `SessionState` message,
and a companion never applies any of them locally: it sends a `Request` and waits
for the broadcast. Being unable to hold a second copy of the value is what makes
"always in sync" true rather than merely likely; a companion that applied
optimistically and reconciled afterwards would have a window where the two ends
disagreed, and reconciliation bugs live in exactly that window.

The same reasoning removed Settings from a connected companion's menu. What is
left in there is that device's own BIOS folder, core path and pointer behaviour,
plus what a new game starts with -- none of it a decision a companion is in a
position to make, since it is not the one running the game. It moved to the
launcher instead, which is where a device that is only ever a screen can still
reach it.

**Why a menu pauses both devices.** Only the host's own menu paused the game, so
opening the menu on the Deck left the game running on the PC -- and left the
Deck's held buttons pressed, since only input *changes* go over the wire. The
companion now reports its menu state, the host pauses while either device has one
open, and the host drops whatever the companion was holding the moment the menu
goes up. The session test holds a button down across the transition, which is the
case that would otherwise walk the character into a wall for as long as the menu
was up.

**Why the sound is a target rather than a switch.** "Send sound to the other
device" could not express the arrangement people actually want -- the Deck in
hand and the TV across the room, with only one of them making noise. Naming the
device by the screen it holds gives three states instead of two, and reads the
same on both devices: "Sound: Top" means the same thing wherever you are looking
at it. The host simply stops sending audio when the companion is not one of the
devices meant to hear it, so the companion needs no rule of its own.

**Why the pointer is a switch rather than three modes.** Hiding on idle and
ignoring `SDL_TOUCH_MOUSEID` is the right behaviour where SDL can tell a finger
from a mouse. Steam Input can present a touchscreen as an ordinary mouse, and
then nothing can: the events are identical. That argued for offering the states
outright -- but a "never hides" one was never really on offer, since SteamOS
hides an idle pointer itself whatever this app asks for. What is left is on and
off: on is the idle-hiding behaviour, off is no pointer at all. The old "auto"
key still reads back as on, so a settings.json written by the three-way version
keeps working.

**Why the footer wraps.** Six buttons across a 1280-wide window leaves each label
about 170px, and "Smoothing: Off" does not fit in that at a readable size. Five
or more wrap onto a second row instead, and focus moves through the two rows as a
grid. Shrinking every label to fit would have kept the code simpler and made the
menu worse.

**Why Settings left the in-game menu, and the pointer took its place.** Nothing
in Settings is a mid-game question: it is folders, the core, and what a new game
starts with. It now opens from the launcher only -- before a game is loaded or a
host is joined -- which is also the answer for a device used only as a screen,
since that is the one screen it always passes through. The pointer went the other
way. It is the one device-local thing worth reaching without leaving the game: a
trackpad you cannot see is a trackpad you cannot aim, and whether the pointer
should be visible depends on what you have in your hands right now.

**Why the pointer mode is read every frame.** It was stored in settings.json,
loaded on startup, offered in the UI -- and never handed to the object that acts
on it, so all three modes behaved as the default and "always hidden" still
auto-hid. The fix is not an extra call at the four places that change it: it is
for the frame loop to read `settings.cursor_mode` directly each frame, so there
is no apply step left to forget. The smoke test starts the app with the mode set
in settings.json and checks it comes up in that mode, which fails against the old
code and would keep failing if the read were ever dropped again.

**Why the layout set is the Wii U's, exactly.** The layouts were invented for
V1 -- vertical, horizontal, their swapped twins, one screen each -- and were
fine, but they were not the thing being copied. The Wii U Virtual Console's six
are: the console drawn around both screens; either screen large with the other
tucked in a corner; both stacked without the shell; both turned sideways; one
per device. They are better thought out than the originals, and the ones this
app was always describing. Old ids from the first set are mapped to the nearest
new one rather than dropped, so a game that remembered `horizontal` opens on
Side-by-Side instead of jumping back to the default.

Left- and right-handed variants are one layout and a toggle here, rather than
the Wii U's ten separate entries. Mirroring is `x -> 1 - x - w` on every
placement, plus reversing any quarter turn; a centred layout comes back
unchanged, which is why the ones with no handed variant on the Wii U need no
special case. The mirror is its own inverse, so the toggle cannot drift.

**Why a layout can carry a frame aspect.** Standard has to keep a real DS's
proportions -- the body is 148.7 x 84.7 mm per half and the screen's active area
is 61.8 x 46.4 mm, which is what makes the screens look small inside it and the
whole thing read as a console. Expressing that as normalized rects in the window
would have stretched the console into whatever shape the display is. So a layout
may name an aspect ratio, its placements are relative to a box of that shape
letterboxed inside the window, and everything downstream -- the compositor, the
previews, the touch mapping -- goes through the same `layout_frame()`. One place
computes it, so they cannot disagree about where a screen is.

**Why the shell is rectangles.** No image ships with this app, and a photograph
of a console would be someone else's artwork. `draw_ds_shell()` is flat quads:
two body halves, a hinge, the wells the screens sit in, speaker grilles, a
d-pad, four face buttons. It aims to read as a DS at a glance rather than to be
a rendering of one, and the same function draws the preview tile, so what the
tile shows is what picking it gives you.

**Why a quarter turn is texture coordinates.** Side-by-Side turns both screens.
Rotating the geometry would have meant a transform in the renderer and a matrix
in the layout; permuting which source corner each destination corner samples
costs nothing and keeps every rect axis-aligned. The turn also swaps which
dimension the letterbox fits against, and it has to be undone when mapping a
touch back -- two pieces of code applying the same rotation in opposite
directions, which is exactly the kind of pair that drifts. The smoke test drives
a touch through the sideways layout and checks the crosshair comes back where
the finger went in, which is what would catch them disagreeing.

**Why the previews are the game.** Two coloured blocks and a key told you which
screen went where, but not what it would look like -- the thing you actually
want to know is whether the touch screen ends up big enough to aim at. The menu
already pauses emulation, so the frames are sitting there; the tiles draw them
through the same placement code the compositor uses. The key survives for the
case where there is no frame yet: a companion that has not received one, or a
core still booting.

**Why layouts describe an arrangement rather than rectangles.** The first pass at
the Wii U's layouts hand-placed normalized rects, tuned by eye against the
screenshots. They were close and all of them were slightly wrong: the two focus
layouts were not aligned to an edge at all, and the gaps between stacked and
side-by-side screens were whatever looked right. Measuring the Wii U's own output
-- a 378 x 212 picture inside the TV frame in its Screen Settings shots -- gives
227 x 170 beside 76 x 57 with 12 px between, 126 x 94 stacked with 18 px, 154 x
204 side by side with 32 px.

Those numbers cannot be written as normalized rects, because a rect's shape
depends on the window's and normalized coordinates stretch with it. So a layout
now carries a composition: an axis, its screens at relative scales with a
cross-axis alignment each, a gap as a fraction of the largest screen's extent,
and a margin. `place_screens()` solves that for the largest scale that fits and
centres the group. Reproducing the measurements to within a pixel or two is a
test, which is the only way to know the layouts match rather than merely
resemble.

The point of describing rather than placing is what comes next. "Two screens in
a row, the second a third the size, aligned to the bottom" is the same idea for a
3DS -- whose screens are 400x240 and 320x240, different sizes *and* different
shapes -- and for a Wii U's TV and GamePad. `place_screens()` already takes the
source dimensions as parameters, so those are new entries in the registry rather
than new geometry code.

**Why the focus layouts align to opposite edges.** It looks arbitrary until you
notice which screen is the small one. In Upper Screen Focus the small screen is
the touch screen, and it sits at the bottom -- where it is on a DS, and where a
thumb reaches it. In Touch Screen Focus the small one is the upper screen, and it
sits at the top, for the same reason. The alignment is a property of the screen,
not of the layout, which is why it is a field on the item rather than on the
composition.

**Why the console is an image now, and how it gets into the binary.** The shell
was drawn from flat quads because no image shipped with the app. Given a real
illustration, the drawn one has nothing to recommend it. The image lives in
`assets/` as an ordinary PNG -- viewable, replaceable -- and CMake turns it into
a source file at build time, so the binary is still the only thing that has to
be copied to a Deck. Decoding it costs a hundred lines against the zlib the
network protocol already links: PNG is chunk headers, one inflate, and undoing
five row filters.

The screens are placed from coordinates measured out of the artwork's own
overlay -- 256 x 192 at (179, 60) and (179, 337) in a 615 x 630 image, one image
pixel per DS pixel. They use Stretch rather than Contain: the rectangles are
already exactly the screens' shape, and letterboxing inside them would leave a
hairline of the illustration showing along an edge. A test checks the placements
against those coordinates *and* that the artwork is dark at the middle of each
one, so a re-exported image that shifted would be caught rather than leaving the
screens sitting slightly off their own wells.

The surround is #252525 rather than black because that is the image's own
background: the picture then has no edge, it simply ends. Smoothing applies to
this layout like any other -- the earlier reasoning, that a blurred screen inside
crisp plastic looks like a mistake, did not survive contact with the real
artwork.

**Why a paused host still sends frames.** Under "Upper Screen on TV" the
companion is only sent the screen it shows. Opening its menu makes it need both,
since the preview tiles draw every layout -- and opening its menu also pauses the
host, so no new frame is ever produced to carry the other screen across. The
companion sat looking at whatever stale copy it happened to have, or at a
coloured block. Publishing now happens whether or not the game advanced, and the
host re-sends the current frame once whenever what the companion needs changes.
Both halves are load-bearing: removing either one leaves the test looking at an
empty screen.

**Why smoothing is decided where a screen is drawn.** It used to be a property of
the textures: the app called `set_filtering` on the source and both screens took
it. That cannot express "smooth this one, leave that one alone", which is what is
actually wanted -- a screen drawn at its own size has nothing to gain from being
resampled, and the same texture can be drawn large in the game and small in a
preview tile in the same frame. Sampling is now a parameter of the draw call, the
`ScreenSource` seam has no opinion about it, and the compositor decides per
screen: the player's setting, unless the screen came out one-to-one.

Spotting "one-to-one" needs a tolerance rather than an equality test. A layout
fitted to a window lands on round numbers only by accident: the focus layouts put
their small screen at 256.5 x 192.4 on a Deck, which is a DS screen in every
sense that matters. Within a pixel counts, and such a screen is then snapped to
whole pixels -- moving it a fraction of a pixel is invisible, while leaving it
half a pixel off the grid resamples it however it is filtered.

**Why a layout can be locked to its own pixel size.** The console artwork is
drawn at 615 x 630 with its screens at 256 x 192. Scaling that to a display
barely bigger resamples both the illustration and the game to gain nothing, so
below a threshold -- a Deck's 1280x800 -- the frame is drawn at exactly its own
size, centred, and allowed over the edges of anything smaller.

This immediately went wrong in the one place a layout is drawn into something
that is not a display. A preview tile is a couple of hundred pixels across, which
is below any such threshold, so the picker drew a full-size console across the
rest of the menu. The lesson was not "remember to clear that field": it was that
"drawn to the display" and "drawn into a box" are different enough to name, so
`as_preview()` strips the display-only parts and a test holds it to that.
