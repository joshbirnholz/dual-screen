# Handoff: Wii U games paused under Dualscreen's quick menu

For a Claude Code session picking up this work. The previous session ran on
the user's Mac and could reach the user's SteamOS PC; a cloud session cannot,
so anything that needs the PC's screen or a real game is the user's to try,
and they will report back. Read all of this before changing anything.

## The two repositories

| Repo | Branch | What it is |
| --- | --- | --- |
| `joshbirnholz/dual-screen` | `claude/emulator-display-streaming-xnfjj0` | Dualscreen: the shell (SteamOS/desktop), the companion client (Switch, Android), CI nightlies |
| `joshbirnholz/cemu-dualscreen` | `claude/dualscreen-frame-link` | The Cemu fork Dualscreen runs Wii U games in |

Develop only on those branches. Don't open pull requests unless the user asks.
CI on every push builds a `nightly` release in each repo; Dualscreen's updater
installs both. Everything described below is pushed.

Commit messages: a short subject, then a body explaining why. End every commit
message with the Co-Authored-By attribution line your Claude Code session
provides. Never put a model name or identifier anywhere in the code, comments
or documentation.

## The user's standing preferences

- Answer questions and bug reports with an explanation and a recommendation;
  change code only when told to ("fix it", "do it", "keep going"). Never end
  with an offer to implement.
- Logs at info level, in words a player would use.
- Controls and values right-aligned in rows. No yellow or orange text.
- Refer to devices by name ("Steam Deck", "Switch"), not by role.
- Dialogs speak in the future tense ("This will delete...").
- Lists: the first tap highlights, the second selects (except layout tiles and
  the screen-swap and stylus-hand buttons). Lists scroll smoothly and don't
  wrap. Holding a direction repeats every 75 ms. Never move the cursor
  unexpectedly.
- B steps back one level. The menu button (or F1) closes everything.
- Run only the tests a change touches while working; the full suite once
  before pushing. Commit and push each fix separately as it's done.
- Comments explain why, at the density of the surrounding code.

## How a Wii U game runs under Dualscreen

Dualscreen starts the Cemu fork (`wii_u_program` in `settings.json`) and talks
to it over the **frame link**: `src/core/external_frame_link.{hpp,cpp}`, a
Unix socket for control messages plus shared memory for pixels. Dualscreen is
the consumer, Cemu the producer. **The fork keeps exact copies of both files
in `src/dualscreen/core/`; when the link changes, copy them across and commit
in both repos.** `external_link_stands_alone` checks they compile alone.

Control messages added in this work (see the header for the rest):

- `Pause` (15), Dualscreen -> Cemu: pause (1) or let it go on (0).
- `PauseState` (16), Cemu -> Dualscreen: `Running`, `Paused`, `Refused`,
  `NotListening`.
- `MenuKeyboard` (17), Cemu -> Dualscreen: an `ExternalMenuKey` (arrows, page
  up/down, confirm, back, F2-F7) going down or up in Cemu's window while the
  menu overlay is up.
- Feature bit `kExternalFeaturePause` in the producer's Hello.

In Game Mode only Cemu's window is visible, so the quick menu is drawn
offscreen by Dualscreen and handed to Cemu, which draws it over the game
(`App::Impl::render_menu_overlay()`, `MenuOverlay`; in the fork
`LatteRenderTarget_drawDualscreenMenu()`). Clicks on it come back as
`OverlayTap`, F1 as `MenuKey`, other keys as `MenuKeyboard`.

## What the pause does now (all done and tested on the PC)

**The pause.** Opening the quick menu over a Wii U game asks Cemu to pause it,
the way the Wii U's HOME Menu does: through ProcUI, the game is sent "release
foreground", stops at a safe point, and waits for "acquire foreground".
- Fork: `coreinit_Misc.cpp` holds the state machine
  (`DualscreenRequestBackground/Foreground`, `DualscreenForegroundReleased`).
  The release goes through the lazy `s_transitionToBackground` flag so an
  undelivered request can be withdrawn; the acquire is sent straight onto the
  system queue (a backgrounded game is blocked receiving), and if asked for
  before the game finished letting go, it waits until it has.
- The GPU thread picks up requests after `DualscreenSession::Poll()`
  (`LatteRenderTarget_handleDualscreenPause()`); the session lives in
  CemuInput's half and cannot call coreinit itself.
- While paused, the GPU thread's idle loop re-presents the last frame at 60 Hz
  with the menu over it (`LatteRenderTarget_dualscreenWhilePaused()`), sleeps
  1 ms between looks instead of spinning, and the main emulated core rests
  0.5 ms between checks (`__OSThreadCoreIdle`). Cemu uses about 10% of a core
  under the menu instead of two.
- Game audio is zeroed while paused (`ax_out.cpp`); verified silent.

**What the menu does with each answer** (`App::Impl::update_wii_u_pause()`):
- `Paused`: the menu opens once the game's picture has arrived (at most
  0.5 s), so there is no dark flash.
- `Refused` (the game has its HOME Menu turned off, e.g. saving): the menu
  stays closed and a Switch-style notice says "The menu can't be opened right
  now." top right for 2.5 s (`src/ui/corner_notice.hpp`). It shows in Cemu's
  window, in Dualscreen's, and on a connected device (session-state field
  `menu_unavailable_notice`).
- `NotListening`: Cemu answers this at once when the game hasn't read a system
  message in the last second (Affordable Space Adventures reads one at boot
  and never again). The menu opens over the running game.
- No answer in 2 s: the menu opens over the running game; the request stands,
  so a late pause still happens under it.
- Setting "Pause Wii U games in the quick menu" (`pause_wii_u_in_menu`, on by
  default) turns all of this off: the menu opens over the live game.

**The background.** On pause Cemu sends both screens' pictures once; the menu
blurs whichever screen Cemu's window shows (`wii_u_backdrop_ready()`), so Swap
Screens while paused swaps the blurred picture, and pages opened from the menu
are 60% darker. A connected device blurs its own last frame once the host's
session-state `paused` flag says the game paused (for Wii U it now means
exactly that). Star Fox Zero's float scan buffer (`B10G11R11`) is now handed
over through the GPU blit.

**Tested on the PC:** Wind Waker HD (title and gameplay), Nintendo Land,
Twilight Princess HD, Bayonetta 2, Breath of the Wild, Hyrule Warriors, Kirby
and the Rainbow Curse, Star Fox Zero all pause in 20-215 ms and resume
cleanly; a one-minute pause was silent throughout. Settings and Data
Management have no layout problems at 4K.

## Still open

- **Never seen on a connected device:** the notice and the blurred background
  on the Switch or Android. Ask the user to try.
- **A real refusal:** only tested with `fake_wii_u --refuse-pause`. A game
  saving would show Cemu's real path. Several games turn their HOME Menu off
  for a second while booting (the fork logs "the game turned its HOME Menu
  off/on"), so a menu press then is refused, as on a console.
- **Wind Waker in gameplay measured 114% CPU while paused**, against about 10%
  on title screens. That session may have been an older build; unconfirmed.
- **Bayonetta 2: F1 reached neither window for a while after the game
  started.** Not investigated; looks like keyboard focus after boot.
- **Keys during the notice:** while the 2.5 s "can't be opened" notice is up,
  the overlay is up, so Cemu holds the menu keys back from the game too.
- **The Switch's slowdown after sleep:** attributed to discovery beacons
  blocking while Wi-Fi reconnects, moved to a thread. Confirm with the user's
  next Switch log ("slow pass ... network" lines).
- **The Emulators page filter** was checked on the PC (shows DS, 3DS, Wii U);
  the Switch/Android side is unchecked.

## Tools and pitfalls for whoever has the PC again

- **Test tool:** `tools/fake_wii_u` stands in for Cemu. `--refuse-pause`
  answers like a game saving; `--ignore-pause` never answers. It offers the
  overlay and the pause, so the menu keys go the way they do with Cemu.
- **The full suite on the PC** always fails `test_video_codec` (the minimal
  FFmpeg has no software encoder) and `test_controller_input` (it sees any
  extra pads attached), and fails `test_session`'s video parts whenever
  Sunshine holds the VAAPI encoder. `test_save_store` fails now and then.
  Build with `tools/build-steamos.sh`, not a bare `cmake --build`, or
  `test_video_codec` loses the FFmpeg headers.
- **`settings.json` gets overwritten:** a running Dualscreen writes its
  settings back from memory on exit. Change `wii_u_program` only while no
  Dualscreen is running, or it reverts (or a test silently runs the installed
  Cemu). Check which Cemu actually started.
- **Cemu's `~/.config/Cemu/settings.xml` is shared** with the installed Cemu.
  Escape in Cemu's window outside the menu leaves fullscreen and Cemu saves
  that; `<fullscreen>` should be `true`.
- **Controls:** Dualscreen uses Nintendo positions, so the east button
  confirms and south goes back. Steam's own UI uses south for A.
- **Screenshots:** `gamescopectl screenshot` sometimes returns a 1920x1080
  capture of a hidden window laid out at 4K (looks magnified). Only the
  3840x2160 captures show what is on screen.
