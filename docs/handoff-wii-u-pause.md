# Handoff: pausing Wii U games under Dualscreen's quick menu

This is for a Claude Code session picking up work from a previous one that ran
in the cloud, without access to the user's machines. The new session runs on
the user's Mac. It can SSH into the user's SteamOS PC, which runs Game Mode,
and it can see that PC's screen (how is being worked out with the user, via
Sunshine/Moonlight). Read all of this before changing anything.

## The two repositories

| Repo | Branch | What it is |
| --- | --- | --- |
| `joshbirnholz/dual-screen` | `claude/emulator-display-streaming-xnfjj0` | Dualscreen: the shell (SteamOS/desktop), the companion client (Switch, Android), CI nightlies |
| `joshbirnholz/cemu-dualscreen` | `claude/dualscreen-frame-link` | The Cemu fork Dualscreen runs Wii U games in |

Develop only on those branches. Don't open pull requests unless the user asks.
CI on every push builds and publishes a `nightly` release in each repo:
SteamOS, Switch and Android for Dualscreen, and a Linux AppImage for the fork.
Dualscreen's updater installs those builds.

Commit messages: a short subject, then a body explaining why. End every commit
message with:

```
Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
```

Never put a model name or identifier anywhere in the code, comments or
documentation.

## The user's standing preferences

- Logs at info level, and say what happened in words a player would use.
- Controls and values right-aligned in rows. No yellow or orange text anywhere.
- Refer to devices by name ("Steam Deck", "Switch"), not by role.
- Dialogs speak in the future tense ("This will delete...").
- Lists: the first tap highlights and the second selects. The exceptions are
  layout tiles and the screen-swap and stylus-hand buttons, which act on the
  first tap. Lists scroll smoothly and don't wrap around. Holding a direction
  repeats every 75 ms. Never move the cursor unexpectedly.
- B steps back one level. The menu button (or F1) closes everything and
  returns to the game.
- While working, run only the tests a change touches. Run the full suite
  (about 6 minutes) once before pushing.
- Commit and push each fix separately as it's done. The user often has little
  time and wants as much pushed as possible.
- Comments explain why, at the density of the surrounding code. Match the
  existing style.

## How a Wii U game runs under Dualscreen (current state)

Dualscreen doesn't emulate the Wii U. It starts the Cemu fork (path in
`settings.json`, `wii_u_program`) and talks to it over the **frame link**.

- **The frame link** (`src/core/external_frame_link.{hpp,cpp}`): a Unix socket
  for control messages, plus a memfd of shared memory for pixels, with seqlock
  slots. Dualscreen is the *consumer* (`ExternalFrameLink`); Cemu is the
  *producer* (`ExternalFrameProducer`).
- **Vendored copies in the fork:** the fork keeps exact copies in
  `src/dualscreen/core/`. **When you change the link, copy both files across
  and commit them in both repos.** The test `external_link_stands_alone`
  checks the files still compile on their own.
- **Control messages so far:** Hello (with a trailing `features` word),
  Frame, Audio, Input, PlayerInput, Rumble, RequestScreen, Reset, and the
  newer ones:
  - `Overlay` (11): Dualscreen → Cemu, "draw this picture over your window"
  - `MenuKey` (12): Cemu → Dualscreen, F1 was pressed in Cemu's window
  - `Pointer` (13): Dualscreen → Cemu, show or hide the mouse pointer
  - `OverlayTap` (14): Cemu → Dualscreen, a click on the overlay, as
    fractions of its size
- **Feature bit:** `kExternalFeatureOverlay` in the producer's Hello.
- **The overlay:** an area of the shared memory after the frame slots (2
  slots, up to 1920×1080, RGBA with straight alpha).

### The quick menu over Cemu's window

Steam Deck Game Mode shows one window at a time. So when a Wii U game's menu
is up, Dualscreen draws the menu offscreen and hands the picture to Cemu,
which draws it over the game.

- **Shell drawing:** `App::Impl::render_menu_overlay()` in `src/app.cpp` and
  `MenuOverlay` in `src/render/menu_overlay.*`.
  - It's laid out at Dualscreen's own window size and drawn into a smaller
    target (`QuadRenderer::begin_frame(w, h, target_scale)`), so both copies
    scroll the same.
  - It's published only when the pixels change.
- **Fork drawing:** `PrepareDualscreenMenu()` and
  `LatteRenderTarget_drawDualscreenMenu()` in
  `src/Cafe/HW/Latte/Core/LatteRenderTarget.cpp`.
  - Textures are created through the `g_imguiBeforeFrame` hook
    (`Renderer.h`/`.cpp`, called from both renderers' `ImguiBegin`).
    Creating them inside the ImGui render pass made the screen flash.
  - A new texture is drawn starting two frames after it's created.
- **Fork input:**
  - F1 in Cemu's main window becomes MenuKey (`MainWindow::OnKeyUp`).
  - Clicks and touches on the overlay become OverlayTap
    (`DualscreenMenuTakes` in `MainWindow.cpp`) and never reach the game.
  - Cemu's own controller mappings read nothing while the overlay is up, and
    stay blocked until held buttons are released
    (`EmulatedController::held_for_menu`). Without this, the B that closes the
    menu reached the game.
- **What the game does now:** it keeps running under the menu. Dualscreen
  keeps polling the link while a menu page is up (`game_running` /
  `pads_to_game` in the main loop of `app.cpp`) and only holds the pads back.
- **Background:** the live game is shown darkened (`kLiveGameDim` in
  `src/render/menu_backdrop.hpp`), not blurred. That applies on both devices,
  and in the companion client (`companion/source/main.cpp`, `draw_scene`).

**Untested on hardware:** the flash fix, the overlay scaling fix, the beacon
thread, game icons read from the ROM, and the Emulators-page filter. CI passes
on all of them.

## The task: pause the Wii U game while the quick menu is up

The user wants Dualscreen's quick menu to act like the Wii U's HOME Menu: the
game pauses, and the background goes back to what DS/3DS games show. That
means a blurred still of the game, darkened further on pages opened from the
menu (Settings, Controller Order, Data Management).

### Why this approach

Cemu can't freeze a game from outside. It reimplements the Wii U's OS on many
host threads, and the console's clock comes from the host CPU's cycle counter
(`PPCTimer_getFromRDTSC`).

The real Wii U pauses for the HOME Menu *cooperatively*, through ProcUI:
1. The system sends "release foreground".
2. The game stops itself at a safe point and calls `ProcUIDrawDoneRelease`.
3. The game waits inside `ProcUIProcessMessages` until "acquire foreground".

Every licensed game handles this. A game can refuse at a bad moment
(`HomeButtonDenied`).

Cemu implements the game's side (`src/Cafe/OS/libs/proc_ui/proc_ui.cpp`). It
also has a preliminary system side: `coreinit::StartBackgroundForegroundTransition()`
and `UpdateSystemMessageQueue()` in `src/Cafe/OS/libs/coreinit/coreinit_Misc.cpp`.
That sends release-foreground and then immediately acquire-foreground. It's
used when a game opens a system app (`sysapp.cpp`, `nn_olv.cpp`).

### Plan

1. **Link protocol.** Add a feature bit (e.g. `kExternalFeaturePause`) and two
   controls:
   - Dualscreen → Cemu: "pause / resume".
   - Cemu → Dualscreen: "paused / refused / resumed".

   Update both copies of the link files, and extend `tests/test_external_link.cpp`.
2. **Fork: split the transition.**
   - Pausing sends only release-foreground. Resuming sends
     acquire-foreground.
   - Report "paused" once the game has called `ProcUIDrawDoneRelease` or
     `OSReleaseForeground`. Report "refused" on `HomeButtonDenied`.
   - Watch out for `s_isInShutdown` / `data1` semantics in `proc_ui.cpp`.
3. **Fork: mute while paused.** Mute Cemu's audio output as a backstop.
4. **Fork: keep the window presenting while paused. This is the riskiest
   part; do it first and verify it on the device before anything depends on
   it.**
   - Cemu only redraws its window when the game flips a frame
     (`LatteRenderTarget_copyToBackbuffer`, called from `LatteThread.cpp`).
     A paused game flips nothing, so the window and the menu drawn in it
     would freeze.
   - Add a path on the GPU thread that re-presents the last frame (and runs
     the ImGui frame with the overlay) at the display rate while paused.
   - Expect Vulkan swapchain and fence details.
5. **Dualscreen: request the pause.**
   - When the quick menu opens over a Wii U game whose producer offers the
     feature, request the pause.
   - Within about a second, either "paused" arrives or the menu stays live
     and darkened (today's behaviour). Close resumes.
   - Add a setting, "Pause Wii U games in the quick menu", on by default.
6. **Background when paused.**
   - Get one still TV frame when the pause starts. `request_screen()` can
     ask Cemu to capture the TV, but it also steers what a connected
     companion is streamed, so be careful. Or have Cemu send a still along
     with "paused".
   - Blur and darken it with `MenuBackdrop` as DS/3DS do.
   - Make the overlay picture opaque: blurred still + menu, with the extra
     60% darkness on submenu pages.
   - Connected device: its last frames are frozen anyway, so use the blurred
     backdrop path again (drop the Wii U live-dim branch while paused).
7. **Game loop.** While paused, Dualscreen can stop treating the external game
   as running under menus. Revisit the `game_running` / `pads_to_game` change
   in `app.cpp`'s main loop, and keep reading the link so taps, F1 and the
   pause replies still arrive.

### Risks to watch

- **Delay:** a short wait for the game to reach its safe point.
- **Refusals:** a refusal is normal (mid-save); fall back to the live menu.
- **Long pauses:** Cemu has only ever held a game in the background for an
  instant. Watch games that stream audio or use network or timers.
- **Game threads:** worker threads may keep running while paused. Games are
  built for this.

## Building and testing on the SteamOS PC safely

- **Leave the installed copy alone.** Don't touch
  `~/Applications/dualscreen/dualscreen`, which the user's Steam shortcuts
  launch, or the shortcuts themselves.
  - Clone fresh copies of both branches. The user's older clone in their home
    folder is stale.
  - Build there, and add a separate "Dualscreen (dev)" Steam shortcut.
  - Back up `~/.config/dualscreen/settings.json` before pointing
    `wii_u_program` at a dev Cemu build, and restore it when done.
- **Build tools:** the user says the build tools/scripts work on SteamOS for
  both repos.
  - Dualscreen is CMake (`cmake -B build && cmake --build build -j`); tests
    run with `ctest` in `build/`.
  - The fork's first build is slow (vcpkg dependencies).
- **Launching in Game Mode:** `steam steam://rungameid/<id>` over SSH usually
  starts a shortcut in the running Game Mode session. Non-Steam shortcut IDs
  are in `~/.local/share/Steam/userdata/<user>/config/shortcuts.vdf`.
- **Logs:**
  - Dualscreen: `~/.config/dualscreen/` (`dualscreen.log`).
  - Cemu: `log.txt` in its data folder.
  - Dualscreen's `--verbose` flag gives more detail.
- **Seeing the screen** (being decided with the user):
  - `screencapture` of the Moonlight window on the Mac.
  - `sudo ffmpeg -f kmsgrab` on the PC.
  - The Claude desktop app's computer use.
- **Pressing buttons:**
  - Keystrokes into the Moonlight window: Dualscreen takes F1, the arrows,
    Enter and Escape; Cemu takes F1.
  - A virtual Xbox controller over SSH: uinput, e.g. python-evdev, needs
    sudo. Dualscreen and Cemu see it as a real pad, which makes it the most
    dependable way to drive a game.
- **Use only the user's own games.** Nobody else should be using the PC while
  testing.

## Other open items from the previous session

- **Live blur under the Wii U menus:** discussed and deferred. Superseded if
  pausing works, since a paused game gets the blurred still.
- **"Empty space at the bottom of Settings"** and **"Data Management list
  scrolls away from its cursor"**: couldn't be reproduced at 1280×800.
  - Fixed on the theory that the overlay copy was laid out at a different
    size from Dualscreen's own window (a TV above 1080p).
  - Confirm on the device. If either remains, ask for the display resolution
    and which screen it was on.
- **The Switch's slowdown after sleep:** attributed to discovery beacons
  blocking on nifm while Wi-Fi reconnects, and moved to a thread.
  - Confirm with the user's next Switch log. Look for "slow pass ... network"
    lines.
