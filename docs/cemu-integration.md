# Handing a Wii U to Dualscreen

Dualscreen plays a DS and a 3DS by loading a libretro core into itself. It does
not play a Wii U that way, and the reason is not a preference: there is no
usable Wii U core to load. `docs/research-notes.md` has the assessment of the
three Cemu-libretro forks that exist; the short version is that all of them
stopped before the thing that matters here.

So a Wii U is played by Cemu, beside this app rather than inside it, and Cemu
hands its two pictures over. Everything on this side of that handover is
finished and tested. What is missing is a Cemu that speaks it.

This document is what that fork has to do.

## What this app gives you

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

**Pixels are XRGB8888, top row first.** One `uint32_t` per pixel, `0x00RRGGBB`,
host byte order. `stride_pixels` is the row pitch in pixels, so a padded buffer
needs no repacking. If Cemu's readback is BGRA or bottom-up, turn it once here
rather than making the shell guess.

**Both screens of one moment share a serial.** `serial` is what stops the
television from one frame being shown beside the GamePad from another. Any
number that increases will do; the frame counter is the obvious one. Publish
both screens under the same value before moving on.

**Publishing must not block on the GPU.** The pixels have to be on the CPU by
the time `publish` is called, and a synchronous `glReadPixels` on the render
thread will cost more than the encode it feeds. Use whatever Cemu already has
for screenshots or for its own capture path, one frame behind if that is what it
takes: a frame of latency is invisible next to a stall.

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
which is how two sessions on one machine stay apart. The fork should take the
path as a command-line argument or an environment variable rather than hardcoding
it.

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
