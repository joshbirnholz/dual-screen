# Handoff: the deko3d core's graphics glitches

Notes for a Claude Code session running on the owner's PC, on the same network
as their Switch, picking up an investigation started in a cloud session that
could not touch the console. Everything below was learned from the owner's
logs and screenshots, one round trip at a time. Read all of it before
changing anything.

Branch: `claude/emulator-display-streaming-xnfjj0`. Newest commit at handoff:
`1ce7d15` (renderer=soft). Commit to this branch; do not open a pull request
unless the owner asks.

## Start here

1. Check the tools below work: build, send the build to the console, launch a
   game, press buttons, take a screenshot, read the game log.
2. Run the one test still waiting (**renderer=soft**, below) and act on its
   result. Everything after that depends on it.
3. Tell the owner what you found in plain words, with screenshots, after each
   real result. They have been very patient; say what a result means, and
   when you were wrong.

## The problem

Dualscreen is a SDL2/OpenGL ES app (Mesa, via SDL's Switch driver) that, built
with a DS core, plays DS games on the console. The fast core is RSDuck's
melonDS `switch-new` branch, which draws the DS's 2D and 3D on the GPU with
deko3d. `switch/melonds_deko/` wraps it in the libretro interface the app
talks to; the emulator itself is compiled from RSDuck's tree unchanged.

In Dualscreen, that core draws **patches of garbage**, mostly small
rectangles about 16x8 DS pixels:

- Mario & Luigi: Bowser's Inside Story (`/roms/DS/Mario and Luigi Bowser's
  Inside Story.nds`), in the same places every run:
  - Toad Town overworld: a strip of garbage under a big Toad's feet.
  - Battle results screen: a purple bar along the very top of the screen
    with a 16x8 patch every 32 pixels -- half of each sprite in a row of
    32x8 sprites.
  - Battle command menu: the same under the "Jump" / "Punch" label box, and
    garbage beside an enemy's shadow and above the HP display.
  - Bowser in the overworld: a patch on his shell, and along the edge of the
    ground.
  - Also in parts of Mario and Luigi's walking sprites.
- Mario Kart DS: garbled textures along the track walls.

melonDS's own Switch app does **not** show them, on the same console:
Gheovgos's melonDS 7.2.1, and `melonDS-today.nro`, built by
`make -C switch/melonds_deko standalone` from the very same RSDuck source and
toolchain, run clean at a solid 60 fps. Gheovgos 7.2 is RSDuck `switch-new`
(6c6f1e8) plus 58 commits that touch only RetroAchievements and button
remapping, nothing in the renderers. So the same renderer code behaves
differently inside Dualscreen. The difference is in this app's runtime or in
the adapter, not in melonDS.

## What is established

- **The DS's video memory is emulated right, and the GPU has the right copy
  of it at the end of each frame.** `vramcheck=1` (gfx.cpp, namespace
  `vramcheck`) checks, between frames with the GPU idle: melonDS's flat CPU
  copies (`GPU::VRAMFlat_*`) against the DS's VRAM, for 2D BG/OBJ of both
  engines and for 3D textures and texture palettes; and the 2D renderer's GPU
  copies (`VRAMTextureMemory[ABG/BBG/AOBJ/BOBJ]`) against the flat copies.
  On build d6b9eac it found **0 differences in 40 checks while the patches
  were on screen**.
  - It does **not** cover: palettes, extended palettes, OAM, the 3D
    renderer's texture cache images on the GPU, or anything mid-frame.
- **Earlier vramcheck reports of "lost uploads" were false.** Its own readback
  waited on a fence signalled without an L2 flush (`signalFence(fence)`
  defaults to `flush=false` in deko3d) into GPU-cached memory, so the CPU read
  some lines left over from the check before. Every "stale" block was exactly
  one check behind. Fixed in d6b9eac. Do not trust any vramcheck log from
  before d6b9eac.
- **The frame readback had the same mistake, now fixed, and it was not the
  cause.** gfx_read_screens() copies each finished frame into CPU-readable
  memory on a separate deko3d queue (`g_copy_queue`). That memory is now
  `GpuUncached`, and the fence is flushed (d6b9eac). The patches survived the
  fix.
- **The patches are not compiler- or JIT-related as far as tested:**
  `jit=0` (slow) still shows them; `-fno-strict-aliasing` made no difference.
- **The shaders are not it:** `shaders=standalone` loads the deko shader
  binaries out of melonDS's own .nro; no difference.
- `texcache=0` (3D texture cache emptied every frame) still shows patches, but
  they *look different*. That is the only setting that has changed them at
  all.

## What has been tried (none fixed it)

All are keys in `/switch/dualscreen/melonds.ini` (read by the core at load,
`switch/melonds_deko/libretro.cpp` `apply_tuning()`), and rows on the app's
Settings > melonDS page (`src/config/melonds_tuning.*`,
`src/ui/settings_screen.*`). Documented in `switch/README.md`.

| Setting | What it does | Result |
| --- | --- | --- |
| no ini at all | defaults | patches |
| `jit=0` | interpreter | patches, slow |
| `fastmem=0` | | not isolated; jit=0 covers it |
| `pipeline=0` | hand each frame over as soon as drawn, not a frame later | patches |
| `gpu_alone=1` | the app calls glFinish() before each core frame, so Mesa is idle while the core runs (`src/core/libretro_screen_source.cpp`) | patches (tested together with pipeline=0) |
| `texcache=0` | 3D texture cache reset every frame | patches, but different-looking |
| `shaders=standalone` | melonDS app's own shader binaries | patches |
| `gpucache=0` | TextureHeap and DataHeap mapped GPU-uncached (bypassing the GPU's L2) | patches, slower |
| `l2flush=1` | full barrier with L2 flush+invalidate after every upload | patches |
| `settle=1` / `settle=2` | a second copy-engine copy after every upload (small, or reading the destination back) | patches |
| `vramcheck=1` | the check above | diagnostic only |
| `-fno-strict-aliasing` | core built with it (now the default, `OPTIMISE` in melonds_deko/Makefile) | patches |

`gpucache`, `l2flush` and `settle` were all aimed at the false "lost uploads"
and can probably be removed once this is solved.

## The one test still waiting: renderer=soft (build 1ce7d15)

`renderer=soft` ("Graphics drawn by: The CPU (software)", the last row of
Settings > melonDS) swaps the deko3d 2D and 3D renderers for melonDS's own
software ones right after `GPU::InitRenderer()` (libretro.cpp
`retro_load_game`), and gfx_read_screens() takes the frame from
`GPU::Framebuffer[GPU::FrontBuffer][0/1]` instead of the GPU. No deko3d
drawing at all. The game log says `drawing on the CPU with melonDS's software
renderers` when it is on.

- **Patches gone** → they come from deko3d drawing inside this app. Go to
  "If the GPU path is to blame".
- **Patches still there** → not GPU drawing. Look at the emulation as
  Dualscreen runs it, and at how the app shows frames (the GL texture upload
  and composition in `src/`). A raw frame dump (below) settles which.

This path is untested on hardware. It compiles; if it crashes or draws
nothing, fix that first. GPU2D_Soft and GPU3D_Soft were already compiled into
the core but unused; GPU3D_Soft runs threaded (`RenderSettings{true, ...}`)
through `switch/melonds_deko/platform.cpp`'s Thread/Semaphore. Expect it to be
slow in 3D scenes.

## If the GPU path is to blame: ideas not yet tried

The things Dualscreen does that melonDS's own app does not, roughly in order
of suspicion:

1. **A second deko3d queue.** `g_copy_queue` (gfx.cpp) does the readback. Every
   deko3d submission starts with L2 FlushDirty + L2 SysmemInvalidate
   (`Queue::postSubmitFlush`, deko3d `source/maxwell/gpu_base.cpp`). The
   readback is submitted right after `NDS::RunFrame()` returns, while the 3D
   list recorded at VCount 215 may still be running on `EmuQueue`. Try doing
   the readback on `Gfx::EmuQueue` itself, with its own command buffer, so
   there is one channel. melonDS's app does its presentation on a second
   queue too (PresentQueue), but only samples the images, never copies them
   out.
2. **Mesa as a second GPU user in the same process.** gpu_alone=1 keeps Mesa
   idle *during* a frame, but Mesa still runs between frames while deko3d work
   may be in flight or sitting dirty in L2. A test: a mode that stops all GL
   drawing for a while and dumps the core's frames to the SD card instead, so
   Mesa never runs at all.
3. **`EmuQueue.waitIdle()` before every frame** (`gfx_wait_for_gpu`). It
   exists because of an earlier texture bug (see its comment). It changes when
   deko3d's own submissions happen compared with melonDS's app.
4. **The data vramcheck does not cover:** palettes (uploaded through
   `UploadBuf.UploadData` and copied per compose region in `ComposeBGOBJ`),
   OBJ extended palettes, the per-batch sprite vertex data (`FlushOBJDraw`
   uploads `SpriteSpec`s through UploadData), and the 3D texture cache's
   TexArray images. The 16x8-every-32-pixels pattern points at sprites, so
   OBJ tiles, OBJ palettes and sprite attributes come first.
5. **Mixing renderers to split 2D from 3D.** The deko3d 2D renderer can take
   a CPU-drawn 3D layer (`GPU2D_Deko.cpp` ~line 559 uploads `_3DBuffer` into
   `_3DFramebuffer`). deko3d 2D with software 3D would show whether the 3D
   side is involved. The reverse is harder: the deko 3D renderer's `GetLine()`
   returns a dummy line, and GPU2D_Soft needs real lines.

## How a frame flows (deko3d path)

- `retro_run` (libretro.cpp): apply input → `gfx_wait_for_gpu()` (waitIdle,
  then vramcheck if on) → `NDS::RunFrame()` → `gfx_read_screens()` → video
  callback → audio.
- RunFrame covers lines 0..262; `GPU::FinishFrame` flips `GPU::FrontBuffer`
  before RunFrame returns.
- The 2D renderer (`GPU2D_Deko.cpp`) records into `Gfx::EmuCmdBuf` through a
  `CmdMemRing<8>` (2 MB slices in DataHeap). It opens its list at line 262
  (`DrawSprites(0)`, sprites for the next frame's line 0), records uploads
  and draws through line 191, then at engine B's line 191 composes both
  screens into `FinalFramebuffers[FrontBuffer^1]`, signals
  `FramebufferReady[FrontBuffer^1]`, submits and flushes. Engine A's compose
  waits on `FramebufferPresented[FrontBuffer^1]` first.
- Uploads go through `UploadBuffer` (three 16 MB staging segments in DataHeap,
  CPU-uncached), then a copy-engine `copyBuffer` into the texture heap. This
  adapter builds its own copy, `switch/melonds_deko/upload_buffer.cpp`
  (upstream's line for line, plus vramcheck and settle hooks).
- The 3D renderer (`GPU3D_Deko.cpp`) records and submits its whole list at
  VCount 215 (`CmdMemRing<2>`, 128 KB slices). It shares `EmuCmdBuf` and
  `EmuQueue` with the 2D renderer; the 2D list is closed then.
- gfx_read_screens(): on `g_copy_queue`, wait `FramebufferReady[front]`, copy
  both final images to `g_readback`, signal `FramebufferPresented[front]`, and
  signal `g_copied[slot]` with flush. With `pipeline=1` (the default) it hands
  over the previous frame's copy, so the CPU never waits on this frame.
- Heaps (gfx.cpp `gfx_init`, same sizes and flags as melonDS's
  `frontend/switch/Gfx.cpp`): TextureHeap 120 MB (Image), ShaderCodeHeap
  12 MB, DataHeap 256 MB (CPU-uncached). `gfx.cpp` includes
  `GPU2D_Deko.h` with `#define private public`, to reach the renderer's
  insides for vramcheck.

Upstream files worth reading in RSDuck's tree (`switch/cores/melonds-deko/src`):
`GPU2D_Deko.cpp` (`UploadVRAM`, `DrawSprites`, `FlushOBJDraw`,
`ComposeBGOBJ`, `DrawScanline`), `GPU3D_Deko.cpp`, `GPU.cpp` (the per-line
calls and `FinishFrame`), `frontend/switch/{Gfx.cpp,main.cpp,UploadBuffer.cpp,
CmdMemRing.h}` (how melonDS's own app does it). deko3d's
`source/dk_queue.cpp` and `source/maxwell/gpu_base.cpp` show what a submit
and a fence actually do.

## The other open problem: a freeze after sleep

The owner put the console to sleep while a game was **still starting**. The
game log stopped right after `hosting MARIO & LUIGI ... on port 47821`, never
logged its first frame (no `this window waits for its refresh` line, which
the first pass of the main loop writes), and next said `the console asked
this app to close` when the owner closed it from HOME. Not investigated
further. Sleep needs the power button, so the owner has to do that step.
Find out first whether it also happens when sleeping mid-game. Relevant:
`src/app.cpp` main loop (`appletMainLoop`, `kSuspendGapSeconds`),
`companion/source/local_play.cpp` (`CpuBoost` applet hook and clock
watchdog; `appletLockExit`).

## Tooling on the PC and the console

What the owner offered: a capture card, controller input, and remote access to
logs. Probably the least setup:

- **Build:** devkitPro with `switch-dev`, `deko3d` and `uam`. From `switch/`:
  `make -C melonds_deko -j8 && make` the first time, then `make`. RSDuck's
  tree lives at `switch/cores/melonds-deko` (git-ignored). The output is
  `switch/Dualscreen.nro`. The third line of every log says which commit was
  built (`this build is <hash>`); check it every time, because the owner once
  tested an old build without knowing.
- **Desktop build and tests** (no console needed), from the repo root:
  `cmake -S . -B build && cmake --build build -j6`, then
  `cd build && ctest -j4 --timeout 300` (69 tests, about 6 minutes). Run them
  after any change outside `switch/melonds_deko/`.
- **Getting a build onto the console:** an FTP server on the Switch (ftpd or
  sys-ftpd) to put it at `sdmc:/switch/Dualscreen.nro`. The app restarts
  itself from that path when going from its menu into a game (`starting again
  as: "sdmc:/switch/Dualscreen.nro" --browse`), so the copy on the card must
  always be the new one. nxlink (devkitPro) can launch it too, and the app
  calls `nxlinkStdio()`, so `nxlink -s` shows the log live; check what
  `argv[0]` becomes, and use `-p switch/Dualscreen.nro` if the restart goes
  wrong.
- **Straight into a game:** start the .nro with the ROM as an argument, plain
  or after `--game`, for example
  `--game "sdmc:/roms/DS/Mario and Luigi Bowser's Inside Story.nds"`.
- **Full memory:** the core needs about 484 MB for the GPU. The homebrew
  menu must be started over a game (hold R while launching one), not from the
  album. A good log says `2909 MB free`.
- **Logs** (fresh each run, flushed per line): `/switch/Dualscreen.log` (menu)
  and `/switch/Dualscreen-game.log` (the game). melonDS settings:
  `/switch/dualscreen/melonds.ini`. BIOS and firmware: `/switch/melonds/`.
- **Buttons and screenshots without a capture card:** sys-botbase (an
  Atmosphère sysmodule, TCP port 6000, plain-text commands ending in `\r\n`:
  `click A`, `press`/`release`, `setStick LEFT x y`, and `pixelPeek`, which
  returns the current screen as a hex-encoded JPEG). Check the README of the
  version installed. It has no password: keep it on the home network.
- **Telling a patch from a real sprite:** JPEG screenshots blur small
  details. The surest evidence is the core's own output. Consider a debug
  option that writes a frame's raw RGBA from gfx_read_screens() to the SD
  card on a button combination or every N seconds, then fetch it over FTP and
  compare. That also settles whether patches are in the core's output at all
  or appear after it, in the GL upload and composition.
- **Save near the glitches:** ask the owner for a save point or a save state
  (the app's quick menu has save states) next to a glitch, so a test does not
  start with playing through the game.

## Working with this owner and repository

- Commit with clear messages to `claude/emulator-display-streaming-xnfjj0`;
  push with `git push -u origin claude/emulator-display-streaming-xnfjj0`.
  No pull request unless asked. No model names in commits, code or docs.
- Code style: match the surrounding code. Comments explain why, in full
  sentences; log lines are plain English at info level (`log_info`), as
  they appear in the game log.
- Keep `switch/README.md` true: it documents every melonds.ini key. When a
  theory turns out wrong, say so there and in the commit, rather than leaving
  the old claim.
- The owner's standing UI preferences, for any UI change: controls
  right-aligned; no yellow or orange text; devices called by their names;
  dialogs in the future tense ("will"); lists move a highlight then select,
  scroll smoothly and do not wrap (except the quick menu's left column,
  which wraps on a press but stops at the ends while held); hold-to-repeat
  every 75 ms; never move the cursor unexpectedly.
- When unsure whether a result means what you think, say so. Several rounds
  here were lost to a diagnostic that misled (vramcheck's readback) and to a
  test on the wrong build.
