# Dualscreen on the Wii U GamePad (work in progress)

This directory is the integration point for a distinct Dualscreen homebrew app
and the desktop host's GamePad transport. It is not a Wii U console title: the
GamePad boots its Homebrew Menu and connects over Wi-Fi directly to the computer.

## Design

The GamePad has a dedicated H.264 decoder with a measured packet format and
refresh cadence. `libwupstream` implements that transport and is substantially
safer to reuse than reimplementing the GamePad's decoder protocol. The host
adapter in `src/net/wup_stream_bridge.*` wraps its C API and translates the
GamePad's buttons, sticks and touch into `DsInputState`.

The host integration should attach this output to the same point where the app
publishes its game screens and audio. It must choose the GamePad screen, scale
or composite to 854x480 RGBA, and forward the returned `DsInputState` through
the existing controller/input merge. Menu rendering needs an explicit follow-up:
the current adapter accepts final RGBA frames, but does not yet capture the
host's composed quick-menu scene. This is intentionally called out rather than
silently shipping a GamePad that cannot show the menu.

## Current limitations

- This commit adds the transport adapter only. The desktop app does not yet
  instantiate it or automatically select it when a GamePad appears.
- `libwupstream` is GPL-3.0-or-later and links x264; the resulting integration
  must preserve compatible licensing and provide required notices/source.
- The current `wupstream_open()` accepts the first WUP Stream beacon. It does
  not distinguish a Dualscreen-branded receiver from ordinary WUP Stream.
  Automatic protocol selection needs an identity/claim handshake before
  enabling discovery, so ordinary WUP Stream users are not captured by a
  Dualscreen host unexpectedly.
- Audio must be resampled to 48 kHz stereo before `submit_audio()`.
- GamePad motion values are not forwarded yet: WUP Stream exposes raw sensor
  fields, but a verified conversion to Dualscreen/Cemu units is still needed.
- WUP Stream's encoder requires the GamePad-specific patched x264 build. Follow
  its host build instructions and link `build/host/libwupstream.a` plus its
  platform socket/thread/math dependencies.

## Build the transport library

Build WUP Stream's host library as documented in its repository:

```sh
sh host/build.sh
```

Then add `host/` to the include path and link the resulting
`build/host/libwupstream.a`. The GamePad-side app should be packaged separately
as a `.wup` with `wup-homebrew`'s `libwup/tools/wupbuild.py`; it can reuse the
receiver implementation from WUP Stream while carrying a Dualscreen name and
identity handshake.

A complete end-to-end feature still needs the desktop frame/menu/audio/input
hooks, distinct GamePad discovery/identification, the WUP app target and a
real-hardware test. The adapter is a foundation, not a claim that those pieces
are complete.
