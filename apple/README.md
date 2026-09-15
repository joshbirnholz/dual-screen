# The Apple companion

A Mac being a second screen for a game running on another machine. Everything
the Switch, Android and desktop companions do, including H.264 — it is the same
source files compiled again, not a port of them. What is here is the build and
the bundle.

**macOS only so far.** It builds and runs on an Apple silicon Mac — an M2 gets
an OpenGL 3.3 core context, finds a host on the network and loads its artwork.
It is still developed on Linux, where it cannot be compiled, so anything below
that has not been through a Mac is marked as such: see **What has actually been
verified** at the end.

iOS and tvOS are the reason this lives in its own directory with one source
list and one plist template rather than as a branch of the root build. Neither
is written yet.

## Building it

```sh
brew install cmake sdl2 ffmpeg
cmake -S apple -B build-macos
cmake --build build-macos
open "build-macos/Dualscreen Companion.app"
```

### Rebuilding

Same two commands, after `git pull`. Both of them: the configure step is what
picks up new files and refreshes the build stamp, and it is cheap to re-run.

The bundle is written in place, so it looks the same afterwards whether the
build replaced it or had nothing to do. Three ways a rebuild appears to do
nothing, in the order they are worth checking:

- **The app was still running.** This is the one that fools everybody, because
  it is two problems wearing one coat. The linker replaces the file by
  unlinking it and writing a new one, which succeeds while the old one is
  running — and the running process carries on executing the old copy, because
  it holds the file it started from. Then `open` on an application that is
  already running just brings it to the front rather than launching the new
  one. So the build genuinely succeeded, the bundle genuinely was overwritten,
  and what is in front of you is still the old program. **Quit it first** —
  ⌘Q, or `killall "Dualscreen Companion"`.
- **There is more than one copy.** `open` takes the path you give it, but a
  copy in `/Applications` and a copy in `build-macos` are two different
  programs and only one of them was rebuilt.
- **The source never changed.** `git pull` first, and check `git log
  --oneline -1`.

Two things say which build is which, and they should agree. The build prints
what it wrote:

```
Built /Users/you/dual-screen/build-macos/Dualscreen Companion.app -- dedc187, configured 2026-09-15 17:39
```

and the app says what it is, in the second line of its log:

```
[info ] Dualscreen starting; log at /Users/you/Library/Logs/dualscreen/dualscreen.log
[info ] this build is dedc187, configured 2026-09-15 17:39
```

If the build says one thing and the app says another, the app you are looking
at is not the one that was just built. `with local changes` after the hash
means the tree had uncommitted edits when it was configured.

If you want to be certain of a clean start, the bundle is only a directory:

```sh
killall "Dualscreen Companion" 2>/dev/null
rm -rf "build-macos/Dualscreen Companion.app"
cmake -S apple -B build-macos && cmake --build build-macos
```

FFmpeg is optional and wanted: without it a Wii U's GamePad screen arrives as
changed tiles at roughly seventeen times the bandwidth (see
`docs/research-notes.md`). Homebrew's build has VideoToolbox in it, which is
the hardware decoder on every Apple machine — the configure step says
`video codec: libavcodec …, decoding through VideoToolbox` when it found one,
and `building without one` when it did not.

`zlib` and `OpenGL` come with macOS. The deployment target is 11.0, the first
release on Apple silicon; override with `-DCMAKE_OSX_DEPLOYMENT_TARGET=` if you
want an older one.

For a universal binary:

```sh
cmake -S apple -B build-macos -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
```

— which needs Homebrew's SDL2 and FFmpeg to be universal too, and they are not
by default. One architecture at a time is the ordinary case.

## Running it

It looks for a host on the local network by itself, the same as every other
companion. `--connect <address>` skips the search.

### The local network

**macOS will ask about the local network**, and the app does nothing at all if
refused — finding a machine on the network *is* the app. The sentence in the
dialog comes from `NSLocalNetworkUsageDescription` in `Info.plist.in`.

A refusal does not look like a refusal. It looks like this:

```
[info ] found host steamdeck at 192.168.0.56 running Lunch
[info ] joining steamdeck (192.168.0.56) as the top screen
[warn ] companion session failed: connect: No route to host
```

Finding the host works, because *receiving* a broadcast somebody else sent is
not something an app has to be allowed to do. Connecting to it is, and macOS
refuses it as EHOSTUNREACH — a sentence that sends you to look at your router,
where there is nothing wrong. The app now says so itself; that log is from
before it did.

If it happens:

- **System Settings → Privacy & Security → Local Network**, and check
  Dualscreen Companion is on. If it is not listed, it was never asked.
- Test the route separately: `nc -vz <host address> 47821` from Terminal.
  Terminal has its own permission, so if that connects and the app does not,
  it is the app's grant and not the network. If *that* fails too, it is a real
  routing problem — a VPN, or a guest network with client isolation on.
- An app is asked at most once per identity, so a bundle macOS has no identity
  for may never be asked. The build signs it ad-hoc for exactly this reason,
  and an ad-hoc signature changes every build, so a rebuild can mean being
  asked again. Copying the app to `/Applications` before the first run gives
  it a stable home as well.

### The log

The log is at `~/Library/Logs/dualscreen/dualscreen.log`, which is also where
Console.app will show it. There is a log at all because a bundle started from
Finder has nowhere to put stdout, and on the evidence of this project the log
is usually the thing that answers the question.

## What is Apple's and what is shared

Almost nothing here is Apple's:

| | |
| --- | --- |
| `CMakeLists.txt` | the build, the bundle, and the icon |
| `Info.plist.in` | what macOS needs told about the app |
| everything else | `cmake/companion_sources.cmake`, the same list the Switch, Android and desktop builds compile |

The handful of places the shared code knows it is on an Apple platform:

- **`src/config/paths.cpp`** — `~/Library/Application Support` and
  `~/Library/Logs`, not `~/.config`. A Mac application writing to a dot
  directory is one somebody has to explain. `PathConvention` is a value rather
  than a set of conditionals precisely so this can be checked from a machine
  that is not a Mac, and `test_config` does.
- **`src/video/video_codec.hpp`** and **`src/video/video_decoder.cpp`** —
  VideoToolbox. It behaves like VAAPI rather than like MediaCodec: a device
  context attached to the ordinary `h264` decoder, not a decoder of its own.
  `h264_videotoolbox` *does* exist as an **encoder**, which is the trap —
  asking for it by name on the decode side finds nothing and reads like a
  build with no hardware decoding in it. `test_video_codec` pins the mapping.
- **`src/net/transport.cpp`** — `MSG_NOSIGNAL` does not exist on Darwin;
  `SO_NOSIGPIPE` on the socket is the same thing done in a different place.
  Naming the flag would not have been a behaviour difference, it would have
  been a name that does not compile. Apple also spells the keepalive idle time
  `TCP_KEEPALIVE` where Linux says `TCP_KEEPIDLE`, and without it a companion
  takes two hours to notice a host that has gone.
- **`src/render/gl_window.cpp`** — a core profile first. Apple has no OpenGL ES
  at all and a legacy profile frozen at 2.1, so a core context is the only real
  option rather than a last resort.

## The icon

Built at build time from `assets/ds_bezel.png` with `sips` and `iconutil`,
both part of macOS, rather than committing a second copy of a picture already
in the repository. That artwork is a photograph of a DS, square-cropped — it is
the right shape and is not really the right picture. A purpose-drawn
`Dualscreen.icns` dropped in beside this file and named in `CMakeLists.txt`
would be better. A machine without those two tools builds a bundle with the
generic icon on it rather than failing.

## What has actually been verified

From Linux, which is the honest limit:

- The whole of `CMakeLists.txt` parses (CMake reads a listfile completely
  before executing it, so a syntax error anywhere would have shown).
- Every file in the shared source list exists.
- `Info.plist.in` expands with no placeholder left behind, and the result
  parses as a real property list.
- **The renderer path macOS will take runs a whole session.**
  `switch_client_gl33_headless` forces an OpenGL 3.3 core context and plays
  through a real host with a screenshot check. Apple's platforms have no GLES,
  so this is the path a Mac uses and it had no coverage at all until this port
  needed it.
- The VideoToolbox name mapping, in `test_video_codec`.
- The `~/Library` layout, in `test_config`.

And on a real Mac, from the log of the first run (an M2, macOS, 3440x1440):

- It compiles, links against Homebrew's SDL2 and FFmpeg, and launches.
- `created a OpenGL 3.3 core context` — the core path, as predicted, and the
  one `switch_client_gl33_headless` covers.
- `video decode: available`, so FFmpeg is linked in.
- The console artwork loads and the panel sizes itself to the display.
- Discovery finds a host on the LAN.
- The log lands in `~/Library/Logs/dualscreen/`.

Still not verified: that VideoToolbox decode actually opens on Apple hardware
(no session has got that far yet), that audio comes out, or that a controller
maps as it should.
