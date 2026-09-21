# The Apple companion

An Apple device being a second screen for a game running on another machine.
Everything the Switch, Android and desktop companions do, including H.264 — it
is the same source files compiled again, not a port of them. What is here is
the build and the bundle.

Four products, three platforms, one source list:

| | | |
| --- | --- | --- |
| **macOS** | desktop OpenGL, Homebrew's SDL2 and FFmpeg | run on a real Mac |
| **iOS** | OpenGL ES, SDL2 built here, touch | **not yet built on a Mac** |
| **iPadOS** | the iOS build exactly — one binary, one bundle | **not yet built on a Mac** |
| **tvOS** | iOS without the touch screen, and nowhere to keep a file | **not yet built on a Mac** |

This is developed on Linux, where none of it can be compiled. Only the macOS
build has been through a Mac. Everything about iOS and tvOS below is written
from the documentation and checked as far as it can be checked from here, which
is further than it sounds but is not the same as having run it: see **What has
actually been verified** at the end, which says precisely which claims have a
device behind them and which do not.

## What each device is good for

The app asks the host for a screen and draws it; which screen is worth asking
for depends on what the device has.

- **iPhone, iPad** — either. A touch screen is what a DS's bottom screen and a
  Wii U's GamePad *are*, so these are the devices that can be the half of the
  console you actually play on.
- **Apple TV** — the top screen, or a Wii U's television. There is no touch on
  an Apple TV and no way in this app to drive a touch screen with a stick, so
  it cannot usefully be a DS's bottom screen. As a television for a Wii U
  session, or the upper screen of a DS game being played on the host, it is
  exactly right.

## Building it

### macOS

```sh
brew install cmake sdl2 ffmpeg
cmake -S apple -B build-macos
cmake --build build-macos
open "build-macos/Dualscreen Companion.app"
```

### iOS and iPadOS

```sh
cmake -S apple -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS \
      -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=XXXXXXXXXX
cmake --build build-ios --config RelWithDebInfo
```

### tvOS

```sh
cmake -S apple -B build-tvos -G Xcode -DCMAKE_SYSTEM_NAME=tvOS \
      -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=XXXXXXXXXX
cmake --build build-tvos --config RelWithDebInfo
```

`CMAKE_SYSTEM_NAME` is not optional and is not guessed. Left out, the build is
a Mac build, and it fails late — in the linker, over a framework that is not
there.

The **Xcode generator** is not optional either for these two. Signing, device
deployment and the simulator all go through Xcode's own machinery, and the
Makefile generator has none of it. Open `build-ios/dualscreen_companion.xcodeproj`
to run on a device or a simulator.

The **development team** is whoever is building, so it is not in the
repository. Without one a simulator build still works; a build for a real
device will not sign. It can also be set in Xcode rather than on the command
line.

#### SDL2 on a device

There is no Homebrew for iOS and no system copy, so SDL is built here as a
subproject. By default it is fetched:

```sh
-DDUALSCREEN_SDL2_TAG=release-2.30.9      # which tag to fetch
-DDUALSCREEN_SDL2_SOURCE_DIR=~/src/SDL    # or build a checkout you already have
```

A checkout is the better answer if you build often — the fetch is a clone per
build directory.

#### FFmpeg on a device, which is the real caveat

On a Mac this is `brew install ffmpeg` and Homebrew's build has VideoToolbox in
it. **There is no equivalent for iOS or tvOS.** FFmpeg has to be cross-compiled
for the device, which is a job in itself, so the build defaults to going
without one rather than failing over a library nobody has yet.

What that costs is not nothing:

- **A DS or 3DS screen is fine.** It carries as changed tiles, which is what
  the Switch companion does today.
- **A Wii U's GamePad screen is not.** Tiles measure at roughly seventeen times
  the bandwidth of H.264 for that screen — see `docs/research-notes.md`. On
  wifi it is not really usable.

So an iPhone or Apple TV built this way is a DS and 3DS companion. For Wii U,
point the build at a cross-built FFmpeg:

```sh
-DDUALSCREEN_FFMPEG_DIR=/path/to/ffmpeg-ios    # expects include/ and lib/
```

The frameworks VideoToolbox needs are linked whenever a codec is found, which
matters more here than on a Mac: every FFmpeg on a device is a static one, and
a static hwaccel's symbols are in the `.a` while its frameworks are not.

There is a better answer than either, which is to decode through VideoToolbox
directly and drop the FFmpeg dependency on Apple platforms entirely. It is a
few hundred lines of Objective-C++ and it is not written, deliberately: it
would be on the critical path of every frame and it cannot be tested from here.

### Rebuilding

Same two commands as whichever platform you configured, after `git pull`. Both
of them: the configure step is what picks up new files and refreshes the build
stamp, and it is cheap to re-run.

The rest of this section is about a Mac, where an app you launched yourself can
still be running. On a device Xcode replaces the installed app each time, so
none of it applies.

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

Every build of this app now says which build it is in the second line of its
log, on every platform -- see `src/util/build_stamp.hpp` for why. On a Mac two
things say it and they should agree. The build prints what it wrote:

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

On a Mac the configure step says `video codec: libavcodec …, decoding through
VideoToolbox` when it found FFmpeg, and `building without one` when it did not.
See **FFmpeg on a device** above for what that costs, which is the same on all
three platforms and much easier to fix on this one.

`zlib` and `OpenGL` come with macOS. The deployment target is 11.0 there and
14.0 on iOS and tvOS; override either with `-DCMAKE_OSX_DEPLOYMENT_TARGET=`,
though on the two devices going below 14 means writing a launch storyboard —
see `Info-iOS.plist.in`.

For a universal binary:

```sh
cmake -S apple -B build-macos -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
```

— which needs Homebrew's SDL2 and FFmpeg to be universal too, and they are not
by default. One architecture at a time is the ordinary case.

## Running it

It looks for a host on the local network by itself, the same as every other
companion. `--connect <address>` skips the search — on a Mac. **There is no
command line on a device**, so on iOS and tvOS the search is all there is, and
a host that discovery cannot find cannot be reached at all. That is a real gap
rather than a quirk of these two platforms; it is the same gap on a direct
Ethernet or hotspot link where a broadcast does not get through.

### Sleep, and the silent switch

Two things a phone and an Apple TV do that a Mac does not, both handled at
start-up in `companion/source/main.cpp`:

- **The screen locks.** Watching a picture is not touching anything, and every
  one of these platforms dims and then locks after a minute or two of that —
  which is exactly the length of a cutscene. `SDL_DisableScreenSaver()`.
- **The ring/silent switch mutes the game.** SDL opens an iOS audio session as
  "ambient" unless told otherwise, and ambient is the category that obeys the
  switch. So a phone with the switch flipped shows the picture in silence and
  there is nothing in the app to look at to find out why. The hint is
  `SDL_HINT_AUDIO_CATEGORY`, set to `playback`, which is what a game asks for.

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

**iOS and tvOS do exactly the same thing**, with exactly the same disguise, and
the sentence comes from `NSLocalNetworkUsageDescription` in `Info-iOS.plist.in`
and `Info-tvOS.plist.in`. Two differences worth knowing:

- The switch is in **Settings → Privacy & Security → Local Network** on iOS,
  and **Settings → General → Privacy & Security → Local Network** on tvOS.
- There is no `nc` on a phone to test the route separately with, so the log is
  the whole of the evidence. It says the same sentence it says on a Mac.

A device build is signed with a real identity rather than ad-hoc, so unlike the
Mac it keeps its grant across rebuilds.

### Where things are written

Not the same place on all three, and on one of them not permanently at all.
`PathStyle` in `src/config/paths.hpp` is where this is decided, as a value
rather than a set of conditionals precisely so the answer for a platform can be
checked from one it is not — `test_config` checks all three.

| | settings and the log |
| --- | --- |
| macOS | `~/Library/Application Support/dualscreen`, log in `~/Library/Logs/dualscreen` |
| iOS, iPadOS | the same shape, inside the app's sandbox container |
| tvOS | `Library/Caches/dualscreen`, **and the system may empty it whenever it wants the space back** |

The tvOS row is not a shortcut. There is no Application Support and no
Documents on an Apple TV; `Library/Caches` is the only place an app may write a
file at all, and Apple's answer for anything that must survive is iCloud
key-value storage, which is a megabyte of preferences and not a place for
files. So an Apple TV can come back having forgotten its settings. For a
companion that is a small loss — it finds hosts by looking rather than by
remembering, and there are no saves on this side of the link, because the game
is running on the other machine.

There is a log at all because a bundle started from Finder has nowhere to put
stdout, and on the evidence of this project the log is usually the thing that
answers the question. On a Mac, Console.app shows it. On a device, reading it
means the container: Xcode's **Devices and Simulators → the device → the app →
Download Container**, or Console.app with the device selected while it runs.

## What is Apple's and what is shared

Almost nothing here is Apple's:

| | |
| --- | --- |
| `CMakeLists.txt` | the build, the bundles, and the Mac's icon |
| `Info.plist.in` | what macOS needs told about the app |
| `Info-iOS.plist.in` | and what iOS and iPadOS do |
| `Info-tvOS.plist.in` | and tvOS |
| everything else | `cmake/companion_sources.cmake`, the same list the Switch, Android and desktop builds compile |

Note what is *not* in that list: there is no Objective-C, no view controller,
no app delegate and no storyboard. SDL2main stands up the UIKit application
object, and the program inside it is the same `companion/source/main.cpp` the
Switch and Android builds run.

The handful of places the shared code knows it is on an Apple platform:

- **`src/config/paths.cpp`** — `~/Library/Application Support` and
  `~/Library/Logs`, not `~/.config`. A Mac application writing to a dot
  directory is one somebody has to explain. iOS is the same shape inside its
  sandbox; tvOS is `Library/Caches` and nothing else, because that is the only
  place an Apple TV lets an app write at all. `PathConvention` is a value
  rather than a set of conditionals precisely so this can be checked from a
  machine that is not a Mac, and `test_config` checks all three.
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
- **`src/render/gl_window.cpp`** — which context to ask for, and this is the
  one place where **Apple is two answers rather than one**. macOS has desktop
  GL and no ES whatsoever: a legacy profile frozen at 2.1 and a core profile
  that goes to 4.1, so core is the only real option rather than a last resort.
  iOS and tvOS are the exact mirror image — ES and no desktop GL at all — on
  hardware from the same company. A single `#if defined(__APPLE__)` gets one of
  them badly wrong, and wrong here is not a slow fallback: it is every attempt
  refused, no context, and an app that will not start. So the answer is a
  value, `GlFamily`, and `test_gl_window` asks it about all three from Linux,
  which is none of them.

- **`companion/source/main.cpp`** — the audio session category, the home
  indicator, the Siri Remote, and not letting the screen lock. See **Sleep, and
  the silent switch** above. About twenty lines, all of it start-up.

## The icon

**On a Mac**, built at build time from `assets/ds_bezel.png` with `sips` and
`iconutil`, both part of macOS, rather than committing a second copy of a
picture already in the repository. That artwork is a photograph of a DS,
square-cropped — it is the right shape and is not really the right picture. A
purpose-drawn `Dualscreen.icns` dropped in beside this file and named in
`CMakeLists.txt` would be better. A machine without those two tools builds a
bundle with the generic icon on it rather than failing.

**On iOS and tvOS there is no icon**, and that is deliberate. Those platforms
want an asset catalog rather than an `.icns`, built by a different tool, and a
malformed one *fails the build* rather than degrading to a blank square — so
generating one from here, where it cannot be compiled even once, would be
trading a missing icon for a broken build. tvOS wants more again: a layered
"Brand Assets" stack, which is several images and is genuinely a design job.

Adding one is a couple of minutes in Xcode, at the point where somebody is
looking at the app on a device anyway: **File → New → Asset Catalog**, add an
**App Icon** set, drop the artwork in, and set the target's `ASSETCATALOG_COMPILER_APPICON_NAME`.
An app with no icon builds, installs and runs; it only fails App Store
validation, and neither of these is going to the App Store today.

## What has actually been verified

### macOS

From Linux, which is the honest limit:

- The whole of `CMakeLists.txt` parses (CMake reads a listfile completely
  before executing it, so a syntax error anywhere would have shown).
- Every file in the shared source list exists.
- `Info.plist.in` expands with no placeholder left behind, and the result
  parses as a real property list — now as a test, `apple_plists`, rather than
  by hand.
- **The renderer path macOS will take runs a whole session.**
  `switch_client_gl33_headless` forces an OpenGL 3.3 core context and plays
  through a real host with a screenshot check. macOS has no GLES, so this is
  the path a Mac uses and it had no coverage at all until this port needed it.
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

And from a whole session against a Steam Deck (the Mac as the top screen):

- `video decoder: h264_videotoolbox` — the hardware decoder really does open on
  Apple hardware, which is the thing this port was least able to check from
  Linux. Decode came in at 3-4ms a frame for 1280x720 with no frames lost.
- Audio plays, and the queue reports its own depth every second, so the drift
  control is running on a real device.

Still not verified: that a controller maps as it should. No pad was attached
for that session.

### iOS, iPadOS and tvOS

**Nothing has been compiled, and no device has run any of this.** There is no
Xcode on a Linux machine, so the build for these two platforms has never been
executed once — not to completion, not to the first error. Read the list below
as "these are the things that were got right on purpose", not as "these work".

What *is* checked, by tests that run on every build:

- **`apple_plists`** — all three property lists. Every placeholder is one the
  build actually substitutes; no XML comment contains `--` (which is not
  well-formed, and rejects the whole file); `NSLocalNetworkUsageDescription`
  is present on all three; iOS has a launch screen and its orientations. Where
  `xmllint` happens to be installed it also parses them for real. Each of those
  checks was confirmed to fail when the thing it looks for is broken — two of
  them because they had already caught something here.
- **`test_gl_window`** — that iOS and tvOS are offered OpenGL ES and never
  desktop GL, that macOS is offered desktop GL and never ES, and that the order
  and the ES flags are right in each case. This is the change that mattered
  most: the code said "Apple has no OpenGL ES at all", which is true of macOS
  and is the opposite of the truth on the other two.
- **`test_config`** — that a tvOS build writes to `Library/Caches` and not to
  an Application Support directory that does not exist there.

What is reasoned from documentation and **not** checked anywhere:

- That SDL2 builds clean as a subproject for both platforms at the pinned tag,
  and that the target names this looks for are the ones it exports.
- That the frameworks linked are the right set and the complete set. The
  simulator and a real device do not always agree about this.
- That `UILaunchScreen` as an empty dict really does avoid the letterboxing, on
  the deployment target set here.
- That the Siri Remote arrives as a `MicroGamepad` this app's menus can be
  driven with, and that the Menu button does something sensible rather than
  quitting from the middle of a session.
- Touch on an iPhone or iPad reaching the host's touch screen correctly. The
  code path is the one Android uses, which is exercised, but the mapping from a
  device's panel to the console's screen has never been seen on these.
- Anything at all about how it performs. The Mac decodes in 3-4ms; an iPhone
  should be comparable and an Apple TV slower, and neither of those is a
  measurement.

### The first things to check on a Mac with Xcode

Roughly in the order they are likely to bite:

1. It configures at all — `cmake -S apple -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS`.
2. SDL2 fetches and builds for the platform.
3. It links. This is where a missing framework shows up.
4. It launches in the simulator and draws the launcher at the full screen size
   rather than letterboxed.
5. The log says `created a OpenGL ES 3.0 context` — if it says anything about
   desktop GL, `GlFamily` is being answered wrongly.
6. On a real device on the same wifi: discovery finds the host, and the local
   network prompt appears. The simulator shares the Mac's network and its
   permissions, so it will not exercise this.
