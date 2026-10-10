# Handing a Wii U to Dualscreen

Dualscreen plays a DS and a 3DS by loading a libretro core into itself. It does
not play a Wii U that way, and the reason is not a preference: there is no
usable Wii U core to load. `docs/research-notes.md` has the assessment of the
three Cemu-libretro forks that exist; the short version is that all of them
stopped before the thing that matters here.

So a Wii U is played by Cemu, beside this app rather than inside it, and Cemu
hands its two pictures over. Everything on this side of that handover is
finished and tested.

The fork that speaks it now exists: **`joshbirnholz/cemu-dualscreen`, branch
`claude/dualscreen-frame-link`**, forked from Cemu at `5ead580`. It has not been
built or run -- there is no GPU where it was written -- so expect a round of
compile fixes. `src/dualscreen/README.md` in that tree is its own account of
itself; **What the fork actually does** below is the summary from this side.

This document is what such a fork has to do, and what this one did.

## What this app gives you

Cemu is MPL-2.0 (its `LICENSE.txt`; the README excepts `dependencies/` and some
individual `src/` files, which carry their own). That is file-level copyleft:
modifying it privately costs nothing, and distributing a binary means publishing
the modified files under the same licence. The files below are this project's
and carry this project's licence, whatever that turns out to be.

Three files, and nothing else from this repository:

    src/layout/screen_id.hpp          the two-line enum the API takes
    src/core/external_frame_link.hpp
    src/core/external_frame_link.cpp

They depend on the C++17 standard library and Linux (`memfd_create`, `SCM_RIGHTS`,
`AF_UNIX`). No logging, no allocator, no build system of ours, nothing else of
this app's. Drop them into a fork, compile the `.cpp`, and `ds::ExternalFrameProducer`
is available.

The producer half is deliberately in the same file as the consumer half. A
protocol described in two places is a protocol with two subtly different
implementations, and the one that would drift is the one living in somebody
else's tree.

## The calls

    ds::ExternalFrameProducer link;

    // Once, when a game starts.
    ds::ExternalStreamInfo info;
    info.top_width = 1280;      // the television
    info.top_height = 720;
    info.bottom_width = 854;    // the GamePad
    info.bottom_height = 480;
    info.audio_sample_rate = 48000;
    info.frames_per_second = 60.0;
    std::string error;
    if (!link.connect(socket_path, info, &error)) { /* run normally */ }

    // Once per emulated frame, after both pictures exist.
    link.publish(ds::ScreenId::Top,    frame, tv_pixels,  1280, 720, tv_stride,  &error);
    link.publish(ds::ScreenId::Bottom, frame, pad_pixels,  854, 480, pad_stride, &error);
    link.send_audio(samples, frame_count, &error);

    // Once per frame, anywhere in the loop.
    bool reset = false;
    if (!link.poll(reset, &error)) { /* the shell went away; carry on alone */ }
    if (reset) { /* restart the title */ }

    // The pad, once the shell has sent one. Latest-state, not events.
    if (link.have_input()) {
        const ds::ExternalInput& pad = link.input();
        // pad.buttons is one bit per ds::ExternalButton, in that order.
        // pad.left_x/left_y and right_x/right_y are [-32767, 32767], y down.
        // pad.touching, pad.touch_x, pad.touch_y are the GamePad's own pixels.
    }

`tools/fake_wii_u.cpp` is a complete working producer in 200 lines. It is what
the tests drive, so it is known to work rather than believed to.

## Six things the fork has to get right

**Pixels are byte order R,G,B,A, top row first.** One `uint32_t` per pixel,
byte 0 = red -- what a GPU-filled RGBA8 render target already is, on every
backend that matters here (Vulkan's `VK_FORMAT_R8G8B8A8_*`, OpenGL's
`GL_RGBA`/`GL_UNSIGNED_BYTE`). `stride_pixels` is the row pitch in pixels, so
a padded buffer needs no repacking.

Do not turn this into the XRGB8888 a CPU-rendered libretro core sends (byte
order B,G,R,0, alpha dropped) before publishing it. That repack is a full CPU
pass over every pixel of every screen, every frame -- measured on a fork's
first attempt at costing ~700ms of *every second* of wall-clock time on a
discrete GPU, where reading a readback buffer back on the CPU at all turned
out to be far slower than on an APU's shared memory, and repacking multiplied
that cost by walking the whole buffer a second time. The shell already knows
the difference: `ScreenReadback::channel_order` (`ChannelOrder::Rgba` for a
GPU source) says which convention a given screen arrived in, so it uploads
byte order R,G,B,A as-is rather than needing it turned into the other
convention first. A conversion is only needed for what genuinely is not
already 8-bit RGBA -- a packed 10-bit-per-channel scan buffer, or a gamma
mismatch between what the source was stored as and what the destination
expects -- and even then, pack the *result* the same way (R,G,B,0xff), not
back into XRGB8888.

**Both screens of one moment share a serial.** `serial` is what stops the
television from one frame being shown beside the GamePad from another. Any
number that increases will do; the frame counter is the obvious one. Publish
both screens under the same value before moving on.

**Publishing must not block on the GPU.** The pixels have to be on the CPU by
the time `publish` is called, and a synchronous readback on the render thread
will cost more than the encode it feeds. One frame behind is fine -- a frame of
latency is invisible next to a stall, and next to the network hop after it.

Cemu's own screenshot path will not do for this. See **Where this attaches**
below: it exists, it is in exactly the right place, and it stalls by design.

**A screen that does not change need not be published.** The shell keeps the
last frame for each screen. A title whose GamePad is a static map can publish it
once. `--pad-only` on the fake producer exercises the other side of this: one
screen arriving and the other never doing so is a supported state, not a bug.

**Nothing here owns the game.** No ROM path crosses the link, no save state, no
settings. Cemu is configured in its own right and this app never second-guesses
it -- which is also why the shell offers no render scale for a Wii U.

**Once `have_input()` is true, the pad is the shell's.** The fork should stop
reading its own controllers for the GamePad and take this instead, or every
press lands twice. The shell has already merged both players' pads and mapped
the touch into the GamePad's own pixels, which is the thing it is actually
better placed to do: a second device across the room is holding half the
controls, and only the shell knows where its window puts that screen.

Until `have_input()` is true, nothing has been sent and Cemu's own input should
carry on unchanged -- a shell that never sends a pad is a shell that has none.

## Where this attaches

Read off Cemu at 5ead580, so line numbers will drift; the names should not.

**The frame hook already exists and is the right one.**
`Renderer::HandleScreenshotRequest(LatteTextureView* texView, bool padView)` is
a virtual on the renderer base, called from one place --
`LatteRenderTarget.cpp`, in `LatteRenderTarget_copyToBackbuffer`, right after
`DrawBackbufferQuad` -- once per screen per frame, with `padView` already
telling the two apart. It runs *before* `ImguiBegin`, `swkbd_render` and
`LatteOverlay_render`, so what it sees is the game's own render target with none
of Cemu's interface drawn on it. That is exactly the picture this link wants.

**Its readback is not reusable.** Both backends stall, deliberately, because a
screenshot happens when somebody presses F12:

- `OpenGLRenderer::HandleScreenshotRequest` does a synchronous `glGetTexImage`
  straight into a `std::vector<uint8>`.
- `VulkanRenderer::HandleScreenshotRequest` does `vkCmdCopyImageToBuffer`, then
  `SubmitCommandBuffer()` and `WaitCommandBufferFinished(...)` -- a full flush
  and a CPU wait.

Both hand back **RGB24**, not the RGBA8888 this link carries, and both walk
every pixel on the CPU when the source and destination disagree about sRGB.

So the fork attaches at that call site and brings its own readback: a ring of
pixel-buffer objects on OpenGL, a ring of staging buffers and fences on Vulkan,
reading the frame from one or two back. That is the bulk of the work in this,
and it is a well-trodden pattern rather than anything exotic. Metal exists too
and can be left alone.

**Input is one call site.** `VPADController::VPADRead(VPADStatus_t& status,
const BtnRepeat&)` fills the whole emulated GamePad for the game. Filling it
from `link.input()` instead of from `controllers_update_states()` is the change,
and `have_input()` is what decides which.

**Touch lands one divide away.** Cemu maps a normalised position onto the
panel's raw range as `x * 3883 + 92` and `4095 - y * 3694 - 254` (see
`VPADController::update_touch`). This link sends the touch in the GamePad
screen's own pixels, so dividing by 854 and 480 gets you to what that formula
wants.

**One size limit to agree on.** The shared memory holds 1920x1080 per screen and
refuses anything larger. `HandleScreenshotRequest` hands over the game's render
target at Cemu's *internal* resolution, so a fork publishing above 1x would be
refused. Either publish at 1x, or say so and the cap moves -- it is a constant
in ExternalSharedHeader and the memory is allocated from it.

## What the fork actually does

Point by point against the list above:

- **The frame tap** is a new `Renderer::HandleDualscreenFrame(LatteTextureView*,
  bool padView)` called from the same line of `LatteRenderTarget_copyToBackbuffer`
  as `HandleScreenshotRequest`, right after it.
- **The readback** is `VulkanRendererDualscreen.cpp`: three staging buffers per
  screen, the copy recorded into whichever command buffer is already being
  filled, the bytes read on a later frame once `HasCommandBufferFinished` says
  that buffer is done. No fence of its own and nothing that waits. A slot still
  in flight when its turn comes round means that frame is dropped.
- **Serial** is `LatteGPUState.frameCounter`, so both screens of one emulated
  frame share it.
- **Vulkan only.** OpenGL inherits the empty base implementation and hands
  nothing over; the shell sits on its waiting screen saying so. Both backends
  were the plan until it was clear how much of Cemu's Linux use is Vulkan.
- **sRGB** is decided the same way the screenshot decides it, against
  `LatteGPUState.tvBufferUsesSRGB` / `drcBufferUsesSRGB`, so what crosses the
  link matches what is on Cemu's own window rather than being a second opinion.
  `R8G8B8A8_UNORM`/`_SRGB` and `A2B10G10R10_UNORM_PACK32` are handled -- the
  latter turned out to be real rather than hypothetical: Wind Waker HD renders
  through it. Its ten bits are read down to eight per channel with no gamma
  step, since the format has no sRGB-tagged twin to compare against
  `dstUsesSRGB` the way RGBA8 does, and the buffer's own flag is the only
  account of its encoding there is. Anything else is warned about once and
  skipped.
- **The pad** is taken in `VPADController::VPADRead`: with a shell connected,
  `controllers_update_states()` and the whole mapping loop are skipped and the
  buttons, both sticks and the touch all come from the link. A GamePad is
  created at launch if that Cemu has never had its input configured, since a
  shell needs one to be holding. The stick is deadzoned here too (a scaled
  radial deadzone at 0.15, matching Cemu's own controller default) -- the
  shell sends it raw on purpose, expecting a libretro core to deadzone its own,
  and Cemu was never in that loop.
- **The GamePad screen reaches the link without Cemu's own "Separate GamePad
  view" window.** That Cemu setting gates its own on-screen pad window, not
  whether the game bothered rendering a DRC picture at all -- it always does.
  Reaching it without that window still means the texture needs freshening
  first (`LatteTexture_UpdateDataToLatest`), the same call
  `LatteRenderTarget_copyToBackbuffer` always opens with and that a direct tap
  has no other reason to make.
- **The socket** is `--dualscreen-socket <path>`, which is what the default
  `wii_u_arguments` below passes.
- **Size.** Nothing was changed about the 1920x1080 cap, so a Cemu above 1x
  internal resolution will have its frames refused, once, in the log.
- **Audio.** Both streams reach the link from `ax_out.cpp`, tagged by screen
  the same way frames are: TV from `AIInitDMA` as `ScreenId::Top`, DRC
  (GamePad) from `AIInitDRCDMA` as `ScreenId::Bottom` -- the same 48kHz int16
  blocks Cemu hands its own two output devices, sent right alongside them.
  Only when a stream is genuinely stereo, which is Cemu's own default for
  both; a title switched to mono or surround on either gets a log line
  instead of a guess at downmixing. The shell routes each stream by its own
  target (`wii_u_tv_audio_target` / `wii_u_gamepad_audio_target`), so a
  player can send the TV's sound to one device and the GamePad's to another
  independently.
  Cemu's audio clock hurries while an output device is short of sound, so
  only a device whose screen is actually played locally counts towards it
  (`AXOut_update`): one left unfed -- a screen sent over the link, or not
  shown at all -- used to keep it hurrying for good, and the screen that was
  played drifted about a quarter of a second behind its picture.

Verified end to end against a real session (Wind Waker HD, Steam Deck): video,
pad, touch and audio all confirmed correct on real hardware, after two bugs the
build couldn't have caught here -- a 10-bit scan buffer format, and a missing
sRGB encode step traced to Cemu's own output shader, both written up in the
git history on `claude/dualscreen-frame-link`.

Not done there yet: the **reset request**, which arrives and is logged. Reset
stays that way on purpose: the only stop-a-running-title path Cemu has,
`MainWindow::EndEmulation()`, is marked by its own author as unfinished, with
memory leaks and crashes after about three uses, and wiring a shell's Reset
button through that would make repeated presses worse than doing nothing.

## What the shell does with it

It listens on the socket before Cemu starts, so the order the two are launched
in does not matter. Until something connects the window is black; the moment
something does, the shell adopts the sizes and the rate it announced and starts
drawing. When the socket closes the session ends -- an emulator being quit is
how a Wii U session ends, and it is treated as an ordinary stop rather than as a
failure.

From there the screens go wherever the layout says, including out to a second
device over the network as H.264. That path is finished and tested end to end
(`tests/run_wii_u_companion_test.sh`); it has simply never had a real Wii U
frame in it.

## The shell's own controls

None of them are on the pad. A Wii U game uses every control the console has,
including both stick presses, so the shell takes nothing from it: the menu is
**F1** or **Tab**, smoothing is **F2**, and **F3** swaps which screen a lone
device is showing. Steam Input can be pointed at any of them.

Which is also why every button on the pad is free to cross the link.

## The socket

`ds::ExternalFrameLink::default_socket_path()` is
`$XDG_RUNTIME_DIR/dualscreen-external.sock`, falling back to
`/tmp/dualscreen-external-<uid>.sock`. The shell's `--external-socket` moves it,
which is how two sessions on one machine stay apart.

**Take the path as an argument.** The shell starts the emulator itself, because
in Game Mode there is nowhere to start one from by hand, and it fills `{socket}`
and `{game}` into a command the player configured:

```json
{
  "wii_u_program": "/path/to/cemu",
  "wii_u_arguments": ["--dualscreen-socket", "{socket}", "--game", "{game}"]
}
```

Those arguments are the default, because they are the fork's; only the program
has to be set. Whatever a different fork's flags are called, they go there --
nothing on this side assumes anything about them beyond those two substitutions, and no shell
sits between the command and `execvp`, so there is no quoting to get wrong. A
fork that only reads a hardcoded path cannot be run twice on one machine and
cannot be tested against a second session.

The shell also stops it: SIGTERM to the process group when the session ends,
SIGKILL if it is still there three seconds later. A fork should treat SIGTERM as
"the player quit" and save what it would save.

The shell creates the shared memory and passes the file descriptor over the
socket on accept, so there is no name to agree on, nothing left in `/dev/shm`
when either side crashes, and no chance of two sessions finding each other's
buffers.

## What is not carried yet

**A Wii U's two audio streams.** The link carries one. A real Wii U can send
different sound to the GamePad than to the television, and a handful of titles
use it. Adding a second stream is a message kind and a channel index; nothing
about the shape of this changes.

**Anything back towards Cemu except a reset and the pad.** The shell has no way
to ask it to pause, to save, or to load. That has not been needed yet.

**Motion controls and the GamePad's camera and microphone.** No title this is
aimed at needs them, and no device this runs on has anywhere to get them from.
