# The Apple companion

An Apple device being a second screen for a game running on another machine.
Everything the Switch, Android and desktop companions do, including H.264 — it
is the same source files compiled again, not a port of them. What is here is
the build and the bundle.

Four products, three platforms, one source list:

| | | |
| --- | --- | --- |
| **macOS** | desktop OpenGL, Homebrew's SDL2 and FFmpeg | run on a real Mac |
| **iOS** | OpenGL ES, SDL2 built here, touch, VideoToolbox | **not yet built on a Mac** |
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

### iOS, iPadOS and tvOS

**There is no `.xcodeproj` in this repository, and there is not meant to be.**
CMake generates one. The configure step below writes
`build-ios/dualscreen_companion.xcodeproj`, and that is a real Xcode project
you can open, run and debug in the ordinary way — it is just not a file anybody
edits or commits.

The reason is the source list. Every client this app has — desktop, Switch,
Android, and all four Apple products — compiles the same files, read from
`cmake/companion_sources.cmake`, because the whole point of the companion is
that it is the app's own code rather than a port of it. A hand-maintained Xcode
project is a second list of those files, and a second list is a list that drifts
— a file added to the client and forgotten in one build is exactly how the two
ends start quietly behaving differently. So the project is generated from the
one list, and regenerating it is a two-second job.

**Start with a simulator.** It needs no developer account, no device and no
signing, and it is the quickest way to find out whether any of this works:

```sh
# iPhone / iPad simulator
cmake -S apple -B build-ios-sim -G Xcode \
      -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator
cmake --build build-ios-sim --config RelWithDebInfo

# Apple TV simulator
cmake -S apple -B build-tvos-sim -G Xcode \
      -DCMAKE_SYSTEM_NAME=tvOS -DCMAKE_OSX_SYSROOT=appletvsimulator
cmake --build build-tvos-sim --config RelWithDebInfo
```

Then open the generated project and hit Run:

```sh
open build-ios-sim/dualscreen_companion.xcodeproj
```

#### All of them at once

`apple/build-all.sh` configures and builds every platform and puts the
generated projects in one Xcode workspace, which is the part that actually
feels like one app:

```sh
apple/build-all.sh                    # all three, simulators for iOS and tvOS
apple/build-all.sh ios tvos           # only the two named
apple/build-all.sh --device --team XXXXXXXXXX
open build-apple/Dualscreen.xcworkspace
```

One window, three schemes, pick a destination and run. A workspace is a list of
projects and nothing else — no build settings, so there is nothing in it to
drift.

**It is three projects, not one, and that is CMake's limit rather than a
choice.** CMake bakes the platform into a project at configure time:
`CMAKE_SYSTEM_NAME` is one value for one project, so three platforms mean three
configures. Xcode itself can do better — its own multiplatform template makes a
*single* target whose `SUPPORTED_PLATFORMS` covers several destinations — and
CMake cannot express that at all. Hand-writing the Xcode project to get it
would mean a second copy of the source list that every other client reads from
one place, which is the thing this build is arranged to avoid. Three projects
and one workspace is the better trade.

What is genuinely one app is the thing that matters for shipping: **all four
products already share one bundle identifier**, which is exactly what Apple's
Universal Purchase wants. One product, one App Store page, one purchase, with
separate binaries underneath — which is how every multiplatform Apple app
works, including Apple's own. There is no fourth build to add for iPadOS; it is
the iOS one.

**For a real device**, drop the sysroot — `CMAKE_SYSTEM_NAME=iOS` defaults to
the device SDK — and supply a team:

```sh
cmake -S apple -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS \
      -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=XXXXXXXXXX
cmake --build build-ios --config RelWithDebInfo
```

Four things that are not optional, in the order they bite:

- **`CMAKE_SYSTEM_NAME`** is not guessed. Left out, this is a Mac build, and it
  fails late — in the linker, over a framework that is not there.
- **`-G Xcode`.** Signing, device deployment and the simulator all go through
  Xcode's own machinery, and the Makefile generator has none of it.
- **`CMAKE_OSX_SYSROOT`** for a simulator. Without it you get a device build,
  which will not run in the simulator and will not sign without a team — and
  the error talks about code signing rather than about the SDK, which sends you
  looking in the wrong place.
- **A development team** for a device, which is whoever is building and so is
  not in the repository. It can be set in Xcode's own target settings instead
  of on the command line.

**The simulator will not exercise the local network permission**, because it
shares the Mac's network stack and the Mac's grants. Discovery working in the
simulator says nothing about whether it works on a phone.

#### When the build dies in ccache

Not this project's fault, and worth recognising quickly because it stops the
build dead on every file at once:

```
dyld[35796]: Library not loaded: /opt/homebrew/opt/fmt/lib/libfmt.11.dylib
  Referenced from: /opt/homebrew/Cellar/ccache/4.11.3_1/bin/ccache
```

Homebrew upgraded `fmt` and did not rebuild `ccache` against it, so ccache
cannot start. SDL turns ccache on whenever it finds one (`SDL_CCACHE`), so
every compile goes through a binary that will not run. Either:

```sh
brew reinstall ccache          # the fix
```

or, to get moving without touching Homebrew, tell SDL not to use it:

```sh
cmake -S apple -B build-ios-ninja -G Ninja ... -DSDL_CCACHE=OFF
```

#### Running on a simulator without the Xcode generator

The Xcode generator is not the only way to get an app onto a simulator, which
matters because it is the part most likely to be broken by a new Xcode. A
Ninja build produces a real `.app`, and a simulator does not require signing:

```sh
cmake -S apple -B build-ios-ninja -G Ninja \
      -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator \
      -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build-ios-ninja

open -a Simulator
xcrun simctl install booted "build-ios-ninja/Dualscreen Companion.app"
xcrun simctl launch --console booted com.joshbirnholz.dualscreen.companion
```

`--console` puts the app's output in the terminal, which is where the lines
about the GL context and the video decoder come out.

What this does not give you is a debugger or on-device deployment. For those
the Xcode generator is still the route.

#### When configure says SDL needs threads

The most misleading failure this build has, so it is written down. It looks
like this:

```
-- Looking for stdio.h - not found
-- Looking for memcpy - not found
-- Looking for M_PI - not found
-- Performing Test CHECK_CPU_ARCHITECTURE_ARM64 - Failed
-- Performing Test HAVE_PTHREADS - Failed
CMake Error at .../cmake/macros.cmake:36 (message):
  *** ERROR: Threads are needed by many SDL subsystems and may not be disabled
```

**Threads are not the problem, and neither is `stdio.h`.** Every one of those
checks failed, including several that cannot possibly be false on a Mac. What
failed is the probe itself — one real error, buried a hundred lines up:

```
CMake Error: Generator: build tool execution failed, command was:
/usr/bin/xcodebuild -project CMAKE_TRY_COMPILE.xcodeproj build ...
```

`check_symbol_exists()` builds a tiny program and links it into an executable.
Cross-compiling to iOS that means a real app binary, which Xcode will not
produce without a signing identity — so every probe fails, and a failed probe
reads as "this platform does not have it". SDL then stops at the first one it
treats as fatal, which happens to be threads.

The build now sets `CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY` for iOS and
tvOS, which makes the probes compile without linking. If you still see it:

1. **Read the real error, not the summary.** CMake 3.26 and later put it in
   `build-ios-sim/CMakeFiles/CMakeConfigureLog.yaml` — `CMakeError.log` is
   from older CMake and will not be there. Search it for the first failing
   probe and read the compiler command and its output:

   ```sh
   grep -n -A40 'stdio.h' build-ios-sim/CMakeFiles/CMakeConfigureLog.yaml | head -60
   ```
2. **Try Ninja before believing anything about the code.** Measured on CMake
   4.4.3 with Xcode 27: the same CMakeLists, the same SDL and the same flags
   configure cleanly under `-G Ninja` and fail every single check under
   `-G Xcode`. When only the generator differs and one of them works, the
   generator is what is broken — CMake's Xcode generator lags new Xcode
   releases, and Xcode moves first.

3. **Check your CMake against your Xcode, and both against SDL.** This is a
   three-way version problem and any pair of them can be the one that is
   wrong. Xcode moves faster than CMake, and CMake 4 is stricter about old
   projects than anything before it — SDL 2.30 predates it. The build now
   passes `CMAKE_POLICY_VERSION_MINIMUM=3.10` to the SDL subproject for that
   reason, which is a compatibility flag rather than a fix.

   The real fix, if this is what it is, is a newer SDL:

   ```sh
   cmake -S apple -B build-ios-sim -G Xcode \
         -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator \
         -DDUALSCREEN_SDL2_TAG=release-2.32.8
   ```

   Any SDL2 tag works; `git ls-remote --tags https://github.com/libsdl-org/SDL.git 'release-2.3*'`
   lists what there is.
4. **Sidestep the Xcode generator.** Ninja cross-compiles to iOS perfectly well
   and does not use `xcodebuild` for its probes, so it answers "does this
   compile at all" without any of the above:

   ```sh
   cmake -S apple -B build-ios-ninja -G Ninja \
         -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator \
         -DCMAKE_OSX_ARCHITECTURES=arm64
   cmake --build build-ios-ninja
   ```

   What it will not give you is a project to run and debug from, which is what
   the Xcode generator is for. Use it to prove the code compiles, then go back
   to Xcode for the rest.

#### Getting an Xcode project

The Xcode generator is what you want for anything real: choosing a destination,
provisioning, capabilities, Instruments, archiving, submission. None of that
has a command-line equivalent worth using.

It has one problem, and it is not in this repository. **Every configure failure
seen on iOS so far has been inside SDL's own feature checks** — a hundred lines
of "not found" for things like `stdio.h`, ending at the first one SDL treats as
fatal. Those are `try_compile` calls, and under the Xcode generator CMake runs a
`try_compile` by driving `xcodebuild`, which is the part a new Xcode breaks.

So build SDL once, with the generator that works, and hand it over. Then there
is nothing left in the configure that compiles anything, and the Xcode
generator has almost nothing to trip over.

```sh
# Once, per platform+SDK. This is the simulator; see below for a device.
git clone --depth 1 -b release-2.30.9 https://github.com/libsdl-org/SDL.git ~/src/SDL
cmake -S ~/src/SDL -B ~/src/sdl-ios-sim -G Ninja \
      -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator \
      -DCMAKE_OSX_ARCHITECTURES=arm64 \
      -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF \
      -DCMAKE_INSTALL_PREFIX=~/src/sdl-ios-sim-install
cmake --build ~/src/sdl-ios-sim --target install
```

Then the project:

```sh
cmake -S apple -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS \
      -DCMAKE_OSX_SYSROOT=iphonesimulator \
      -DDUALSCREEN_SDL2_PREFIX=~/src/sdl-ios-sim-install \
      -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=XXXXXXXXXX
open build-ios/dualscreen_companion.xcodeproj
```

or through the script, which does all three platforms:

```sh
apple/build-all.sh --xcode --sdl-prefix ~/src/sdl-ios-sim-install
open build-apple/Dualscreen.xcworkspace
```

SDL installs its own CMake package files, so the frameworks a static SDL needs
come with it and are not listed again here.

##### The device-and-simulator catch

A static `libSDL2.a` is built for one SDK. Xcode switches SDK with the
destination, so **one prefix cannot serve both a simulator and a device** — and
switching destination in Xcode will fail to link rather than fail clearly.

Two ways round it, and as an iOS developer you will know which you prefer:

- **Two build directories**, one configured with `iphonesimulator` and one
  with `iphoneos`, each pointed at its own SDL prefix. Blunt, and it works.
- **An `SDL2.xcframework`**, which carries device and simulator slices in one
  bundle and lets Xcode pick per destination — which is what this arrangement
  really wants. SDL's own `Xcode/SDL/SDL.xcodeproj` has a scheme that builds
  one. This build does not wire that up yet: linking an xcframework also means
  getting the header search path right for `#include <SDL.h>`, and that has
  not been tried on a Mac from here. It is the better answer and is worth
  doing.

##### If the Xcode generator still fails

Then it is CMake's Xcode support against your Xcode, and nothing here can fix
it. Two things worth knowing before spending an evening on it:

- The real error is in `build-ios/CMakeFiles/CMakeConfigureLog.yaml`.
  `CMakeError.log` has not existed since CMake 3.26.
- A hand-maintained `.xcodeproj` is the durable alternative, and for an app
  heading for the App Store it may be the right one regardless. The reason
  this repository generates rather than commits one is the source list: every
  client compiles the same files from `cmake/companion_sources.cmake`, and a
  second copy of that list is one that drifts. Xcode 16's file-system
  synchronized groups would answer that, since they reference a folder rather
  than enumerate files.

#### SDL2 on a device

There is no Homebrew for iOS and no system copy, so SDL is built here as a
subproject. By default it is fetched:

```sh
-DDUALSCREEN_SDL2_TAG=release-2.30.9      # which tag to fetch
-DDUALSCREEN_SDL2_SOURCE_DIR=~/src/SDL    # or build a checkout you already have
```

A checkout is the better answer if you build often — the fetch is a clone per
build directory.

#### The video codec

**H.264 decodes through VideoToolbox, with no FFmpeg in the path.** That is
Apple's own decoder, and it is what was doing the work all along: FFmpeg's
`h264` decoder with a VideoToolbox device attached is a wrapper around exactly
those frameworks. What FFmpeg genuinely supplied was bitstream plumbing —
splitting NAL units, lifting the parameter sets out, re-framing the rest — and
that is now `src/video/h264_bitstream.cpp`, a few hundred lines of arithmetic
over a byte array which is tested on Linux against this app's own encoder.

So an iPhone or an Apple TV gets H.264 with nothing installed and nothing
cross-compiled, which is what matters: a Wii U's GamePad screen costs roughly
seventeen times the bandwidth as tiles (see `docs/research-notes.md`), so
without H.264 those devices would be DS and 3DS companions only.

FFmpeg is still optional and still wanted on a Mac, where it is
`brew install ffmpeg`. It is no longer what decodes H.264 there either — the
VideoToolbox path is preferred on every Apple platform — but it is what encodes,
which a Mac being a *host* needs. On a device you can still point the build at
a cross-built one:

```sh
-DDUALSCREEN_FFMPEG_DIR=/path/to/ffmpeg-ios    # expects include/ and lib/
```

There is nothing it will add to decoding.

##### The copy that is still there

The win this does **not** yet take: VideoToolbox hands back a `CVPixelBuffer`
the GPU can already see, and the decoder then copies its two planes into system
memory so the renderer can upload them as textures. Handing that buffer
straight to a texture instead is zero copies, and it is the real performance
change.

It is a separate piece of work because the two platforms need different things:

- **iOS and tvOS** are clean. `CVOpenGLESTextureCacheCreateTextureFromImage`
  gives an ordinary `GL_TEXTURE_2D` per plane, which drops straight into the
  NV12 two-texture path the renderer already has.
- **macOS** is not. IOSurface-backed GL textures there are
  `GL_TEXTURE_RECTANGLE`, which needs a `sampler2DRect` variant of the shader
  and unnormalised coordinates. That is real work in the core renderer, on the
  one Apple platform that currently works.

The buffers are already allocated GL-compatible (`kCVPixelBufferOpenGLESCompatibilityKey`
and the IOSurface properties are set in `videotoolbox_decoder.cpp`), because a
buffer allocated without that cannot be used this way later and the flag costs
nothing now.

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
17.0 on iOS and tvOS; override either with `-DCMAKE_OSX_DEPLOYMENT_TARGET=`.
On the two devices 17 is a choice, not a floor -- Xcode 27 will go down to 15
-- made because this app decodes video, and two more years of VideoToolbox is
worth more to it than two more years of devices. Going below 14 would also
mean writing a launch storyboard (see `Info-iOS.plist.in`).

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

**It does not need the multicast entitlement**, which is worth saying because
it is the first thing anybody worries about here and it is not cheap to get —
`com.apple.developer.networking.multicast` requires Apple's approval, per app.
iOS 14 and later gate *sending* to broadcast and multicast addresses behind it.

This app never does that from a companion. Discovery is one-way: the host
broadcasts a beacon, the companion only ever listens. Everything the companion
sends afterwards is unicast, to an address it learned from a beacon it
received — including the early "knock" that provokes the permission prompt,
which is a single datagram to one host and opens its socket with broadcast
explicitly off (`knock_on_the_local_network()` in `companion_session.cpp`).

The ordinary local-network usage description is all it wants. A *host* on iOS
would be a different question, and there is no iOS host.

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
- **`src/video/videotoolbox_decoder.cpp`** and
  **`src/video/h264_bitstream.cpp`** — H.264 through Apple's decoder with no
  FFmpeg in the path at all. The first is the session; the second is the
  bitstream work FFmpeg used to do, and is plain arithmetic over a byte array,
  so `test_h264_bitstream` checks it on Linux against this app's own encoder's
  output. `no_codec_typecheck` compiles the whole companion the way a device
  builds it — `DS_HAVE_VIDEO_CODEC=0` — which is otherwise dead code to every
  build in this repository.

- **`src/video/video_codec.hpp`** and **`src/video/video_decoder.cpp`** — the
  FFmpeg route to the same place, still there for a Mac and still used for
  anything that is not H.264. It behaves like VAAPI rather than like
  MediaCodec: a device context attached to the ordinary `h264` decoder, not a
  decoder of its own. `h264_videotoolbox` *does* exist as an **encoder**, which
  is the trap — asking for it by name on the decode side finds nothing and
  reads like a build with no hardware decoding in it. `test_video_codec` pins
  the mapping.
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

**No device has run any of this.** But some of it has now been through a
compiler, which is more than was true when it was written, so this section is
worth reading in three parts rather than one.

**What a real Mac has now run** (macOS 27, Xcode 27, CMake 4.4.3, an M2):

- `videotoolbox_decoder.cpp` and `h264_bitstream.cpp` compile clean, the
  frameworks link and the bundle signs. Every CoreMedia and VideoToolbox call
  in them was written from documentation and had never been near a compiler.
- **And it decodes.** `video decoder: VideoToolbox, no FFmpeg in the path`,
  across two sessions against a Steam Deck — a DS game and a Wii U one — for
  just over two minutes: **zero frames lost**, decode 3ms a frame at the
  median and 6ms at its worst, no keyframe storms, no errors. The session
  opens, the format description built from the parameter sets is right, frames
  come back, and the picture looks correct — which also says the full-range
  pixel format was the right choice, since video range would have shown up as
  washed-out colour rather than as a failure.

  This is the thing that could not be checked from Linux at all, and it is the
  same decoder iOS and tvOS will run.

**What generates an Xcode project for iOS**, with SDL handed over prebuilt:

- `cmake -S apple -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS
  -DCMAKE_OSX_SYSROOT=iphonesimulator -DDUALSCREEN_SDL2_PREFIX=...`
  configures and generates. zlib resolves inside the simulator SDK, threads
  resolve to nothing to link, the codec line reads `VideoToolbox only`, and
  `build-ios/dualscreen_companion.xcodeproj` is written.
- So **the Xcode generator was never the problem — SDL's feature checks
  were.** Removing them removed the failure. Worth being precise about,
  because one line of that configure says the underlying fault is still
  there: `Check if compiler accepts -pthread - no`, which is a `try_compile`
  and which should say yes. It is simply no longer depended on.

**What configured for iOS**, under the Ninja generator:

- The whole of `apple/CMakeLists.txt` for `CMAKE_SYSTEM_NAME=iOS`. SDL2 builds
  as a subproject and reports `Platform: iOS` with OpenGL ES on and Cocoa off;
  `zlib` resolves inside the iPhoneSimulator SDK; threads resolve; the codec
  line reads `VideoToolbox only, which is all this needs`.
- The same configure fails every feature check under `-G Xcode` on that
  machine, which localises it to CMake's Xcode generator rather than to
  anything here. See the troubleshooting above.

**What has still never happened:** no iOS or tvOS binary has been linked, no
app has been installed anywhere, and nothing has drawn a frame. Read the list
below as "these are the things that were got right on purpose", not as "these
work".

What *is* checked, by tests that run on every build:

- **`test_h264_bitstream`** — the NAL splitting, parameter-set extraction and
  length-prefixing a VideoToolbox session is fed, run over frames from this
  app's own encoder. This is the half of the native decoder with the bugs in
  it; the session wiring around it is thin. Both halves of it were
  mutation-checked: a wrong-endian length prefix and a missing zero-trim each
  make it fail.
- **`no_codec_typecheck`** — that the companion compiles with
  `DS_HAVE_VIDEO_CODEC=0`, which is how a device builds it. Linux always finds
  FFmpeg, so that branch is compiled by nothing else here. Confirmed to fail
  when that branch is broken while the ordinary build stays green.

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

- That the VideoToolbox decoder behaves on a *device*. It is proven on a Mac
  now, which is the same code — but an iPhone and an Apple TV have their own
  decoders, their own thermal limits and, on tvOS, no touch. Nothing about
  what it does at 1080p on a phone is known.
- That `apple/build-all.sh` runs. Its shell parses and the workspace it writes
  is well-formed XML, which is all that can be checked from here; whether the
  three configures inside it succeed is the same open question as configuring
  them one at a time.
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

1. It configures at all. Use a **simulator** build for this — no team, no
   device, no signing:
   `cmake -S apple -B build-ios-sim -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator`
2. SDL2 fetches and builds for the platform.
3. It links. This is where a missing framework shows up.
4. It launches in the simulator and draws the launcher at the full screen size
   rather than letterboxed.
5. The log says `created a OpenGL ES 3.0 context` — if it says anything about
   desktop GL, `GlFamily` is being answered wrongly.
6. The log says `video decoder: VideoToolbox, no FFmpeg in the path`, and a
   picture actually appears. **Try this on the Mac first** — it is the same
   code there and the Mac is the Apple device that already works, so it is the
   cheapest place to find out whether the decoder is right.
7. On a real device on the same wifi: discovery finds the host, and the local
   network prompt appears. The simulator shares the Mac's network and its
   permissions, so it will not exercise this.
