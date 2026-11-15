# Building Dualscreen

Every platform builds from this one repository. The desktop app (SteamOS,
Linux, Windows) is the full thing: it plays DS, 3DS and Wii U games and can
also join one as a companion. The Switch, Android and iPhone/iPad builds play
DS games and join games running elsewhere; the Switch 2, Apple TV and Mac
builds only join.

## Nightly builds

Every push to the dev branch is built for SteamOS, Windows, the Switch, the
Switch 2 and Android by `.github/workflows/builds.yml`, and they are published
together as the **nightly** pre-release on the repository's Releases page,
replaced each time, with a `version.json` saying which build and commit it is.
The build number is that workflow's run number ("Nightly build N"). Android's
signing key comes from the repository's secrets; see
[android/README.md](../android/README.md).

`.github/workflows/ios.yml` builds the iPhone and iPad app on a macOS runner,
leaving an unsigned `.ipa` as an artifact, and plays a test DS program in the
simulator. It is not part of the nightly release.

## Updates

A build from there knows its own number, and updates itself from the next ones
(`src/update/`). At start it looks at the nightly's `version.json`; when there
is a newer build, the main menu offers it -- **Update** downloads and installs
it, **Not Now** leaves it, and that build is not offered at start again.
**Settings > System** is the one place for everything Dualscreen keeps up to
date: it says which build of Dualscreen this is and
what each emulator it keeps is at, and **Check for Updates** checks all of them
at once -- including on a build made by hand, which never looks on its own.

- **SteamOS and Windows**: the archive is downloaded with the system's `curl`,
  unpacked with `tar` (both come with Windows 10 and 11), and its files are
  put in place of the ones in the folder Dualscreen is running from; then
  Dualscreen starts again. That folder has to be one it can write to.
- **Switch and Switch 2**: the `.nro` is downloaded and written over the one Dualscreen was
  started from, and the homebrew loader starts it again. Forwarders name that
  path, so they start the new build too.
- **Android**: the APK is handed to Android's package installer, which asks
  first -- the first time, for Dualscreen to be allowed to install apps at
  all -- and then replaces it. It only installs over a build signed with the
  same key, so a locally built debug APK has to be uninstalled once first.


## Each platform

| Platform | What you get | Command | Details |
| --- | --- | --- | --- |
| Steam Deck / SteamOS | the desktop app | `./tools/build-steamos.sh` | [below](#steam-deck--steamos) |
| Linux, other desktops | the desktop app | `cmake -S . -B build && cmake --build build -j` | [below](#linux-and-other-desktops) |
| macOS | the companion, as `Dualscreen Companion.app` | `cmake -S apple -B build-macos && cmake --build build-macos` | [below](#macos), [apple/README.md](../apple/README.md) |
| iPhone, iPad, Apple TV | the companion; on iPhone and iPad, a DS player too | `apple/build-all.sh` | [below](#iphone-ipad-and-apple-tv), [apple/README.md](../apple/README.md) |
| Windows | the desktop app, cross-compiled from Linux | `./tools/build-windows.sh --package` | [below](#windows) |
| Nintendo Switch | the companion and a DS player, as `Dualscreen.nro` | `cd switch && make` | [below](#nintendo-switch), [switch/README.md](../switch/README.md) |
| Nintendo Switch 2 | the companion, as `Dualscreen-Switch2.nro` | `make -C switch SWITCH2=1` | [below](#nintendo-switch), [switch/README.md](../switch/README.md) |
| Android | the companion, and a DS player, as an APK | `cd android && ./gradlew assembleDebug` | [below](#android), [android/README.md](../android/README.md) |
| Cemu, for Wii U games | the fork this app hands screens over with | `cmake -S . -B build -G Ninja && cmake --build build` | [below](#cemu-for-wii-u-games) |

After `git pull`, run the same commands again, both configure and build. The
configure step picks up new files and records which build this is. Every build
writes its commit into the second line of its log (`this build is d5eb384 …`),
which is the quickest way to check that a device is running what you just
built.

### Steam Deck / SteamOS

```sh
git clone https://github.com/joshbirnholz/dual-screen
cd dual-screen
./tools/build-steamos.sh
```

That is the whole thing. First run takes a few minutes to fetch the container
image; after that it is seconds. The result is `build/dualscreen`, which runs
**on the SteamOS host** — in Game Mode, with no container at runtime, and it
keeps working across SteamOS updates. If FFmpeg was available to build
against, `build/` also holds a handful of `.so` files the binary needs for
H.264 — see below — so treat `build/` as the artifact to move around (to add
as a Steam shortcut from elsewhere, say) rather than `dualscreen` alone.

<details>
<summary>Why it works this way</summary>

SteamOS's root filesystem is read-only and ships no compiler, so the build has
to happen in a container. The script uses **Debian 12**, which is deliberately
*older* than any SteamOS release: a binary built against an older glibc runs on
a newer one, but not the reverse. Build in an Arch container instead — the
obvious choice, since SteamOS is Arch-based — and you get a rolling-release
glibc newer than the Deck's, and a binary that will not start.

That direction matters more than it sounds. Built on glibc 2.39, this binary
demands glibc ≥ 2.38, entirely because GCC 13 quietly rewrites `strtol` and
friends into `__isoc23_strtol`. Nothing in the source needs anything that
recent. Build it on Debian 12 and the floor drops to what the code actually
uses.

The other half is what the binary needs at all:

```
$ objdump -p build/dualscreen | grep NEEDED
  NEEDED   libSDL2-2.0.so.0
  NEEDED   libm.so.6
  NEEDED   libc.so.6
```

SDL2 and libc, and that is it. Two reasons:

- `libstdc++` is linked statically (`DUALSCREEN_PORTABLE`, on by default),
  since the C++ ABI is the coupling most likely to break when a container-built
  binary meets the host's runtime. Costs about 1.4 MB.
- **`libGL` is absent**, even though CMake links it. Every GL entry point is
  resolved through `SDL_GL_GetProcAddress` at runtime, so the linker drops the
  direct dependency and there is no GL/EGL version coupling to get wrong. This
  is the same property that lets the renderer take an OpenGL ES context on one
  machine and desktop GL on another.

FFmpeg is the one exception to "SDL2 and libc, and that is it," and it ships
no static archives the way libstdc++ does, so a binary built with H.264
support genuinely needs `libavcodec.so`, `libavutil.so` and `libswscale.so`
at runtime. Debian's own `libavcodec-dev` was the obvious way to get them,
but a stock build of it links dozens of codecs this project has no use for
— AV1, HEVC, Theora, JPEG XL (which alone drags in librsvg, libcairo and
glib) — every one of which the SteamOS host would then also need too, or
this script would have to bundle. `build_ffmpeg_if_needed` in
`tools/build-steamos.sh` builds FFmpeg from source instead, configured with
nothing but the h264 decoder/parser and the h264_vaapi encoder/hwaccel (see
**What streaming a screen costs**, and `src/video/video_encoder.cpp`/
`video_decoder.cpp` for what actually gets called). The result is three
small libraries whose own dependencies — `libva`, `libdrm`, `libX11` — are
exactly the "any Linux desktop with GPU acceleration already has this"
class of library `libGL` above is already left unbundled for; `libx264` is
deliberately not built at all, keeping this LGPL rather than GPL. Cached
under `.ffmpeg-steamos/` at the repo root (separate from `build/`, so
`--rebuild` does not force redoing a from-source FFmpeg build every time),
`bundle_ffmpeg_libs` copies just those three files into `build/` next to
the binary, and the binary's own `$ORIGIN` rpath (set in `CMakeLists.txt`
when FFmpeg is found) is what makes it look there before giving up. A build
without FFmpeg available to configure against copies nothing and needs
nothing extra, same as before.

**Do not use `sudo steamos-readonly disable`** to install a compiler on the
host. It works until the next SteamOS update wipes it, and then it works again
until the update after that.

</details>

### Linux and other desktops

Needs a C++17 compiler, CMake 3.16+, SDL2 and nlohmann::json.

```sh
sudo apt install build-essential cmake libsdl2-dev nlohmann-json3-dev  # Debian/Ubuntu
sudo pacman -S base-devel cmake sdl2 nlohmann-json                      # Arch

cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build --output-on-failure
```

If nlohmann::json is not installed, CMake fetches it. `build-steamos.sh` also
detects a non-SteamOS machine with a compiler and just builds directly, so it
is safe to use everywhere.

H.264 encoding (see [technical.md](technical.md#what-streaming-a-screen-costs)) needs FFmpeg's
dev packages found by `pkg-config` at configure time:

```sh
sudo apt install libavcodec-dev libavutil-dev libswscale-dev  # Debian/Ubuntu
sudo pacman -S ffmpeg                                          # Arch
```

Without them, CMake prints `video codec: not available, building without one`
and carries on -- a build like that sends every screen as tiles, which is fine
for DS and costly for a Wii U GamePad or a 3DS at a raised render scale.

Useful CMake options: `-DDUALSCREEN_PORTABLE=OFF` (link libstdc++ dynamically),
`-DDUALSCREEN_BUILD_TESTS=OFF`, `-DDUALSCREEN_WERROR=ON`.

### macOS

The Mac build is the companion app. It joins a game running on another device
rather than running one itself.

```sh
brew install cmake sdl2 ffmpeg
cmake -S apple -B build-macos
cmake --build build-macos
open "build-macos/Dualscreen Companion.app"
```

**Quit the running app before rebuilding** (⌘Q, or `killall "Dualscreen
Companion"`). Otherwise `open` brings the old copy to the front, and it looks
as though the rebuild did nothing. For a universal binary, add
`-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"` to the first `cmake`. More in
[apple/README.md](../apple/README.md).

### iPhone, iPad and Apple TV

Xcode projects are generated by CMake rather than committed.
`apple/build-all.sh` builds every Apple platform and puts the projects in one
workspace:

```sh
apple/build-all.sh                              # simulators for iOS and tvOS, plus macOS
apple/build-all.sh --device --team XXXXXXXXXX   # for real devices, signed with your team
open build-apple/Dualscreen.xcworkspace
```

Then pick a target in Xcode and press Run. A real Apple TV needs SDL built
for tvOS first; [apple/README.md](../apple/README.md) has that, and the ways a
device build tends to fail.

An Apple TV draws at 1080p even on a 4K television, which scales it up, and
asks the host for pictures up to 1920x1080. A Wii U game arrives at whatever
size Cemu renders its TV screen at: 1280x720 for most games as they are, or
the full 1920x1080 with a Cemu graphic pack that raises the TV resolution to
1080p. Drawing at 4K filled four times the pixels every frame for detail no
stream carries. An iPhone or iPad draws at its full resolution.

### Windows

Every nightly release has `dualscreen-windows.zip`. Unzip it anywhere and run
`dualscreen.exe` from its `Dualscreen` folder, or add that to Steam. It is
the whole app: it hosts games, joins as a companion, updates itself, fetches
cores and Cemu (`Cemu-dualscreen-windows-x64.zip`, from the fork's nightly),
syncs saves, and runs the Remote Start service (Settings turns it on, and it
starts with Windows from then on). Settings live in `%APPDATA%\dualscreen`;
saves, cores, Cemu and the log live in `%LOCALAPPDATA%\dualscreen`.

The build is cross-compiled from Linux with MinGW-w64, the same way in CI as
on a developer's machine:

```sh
sudo apt install g++-mingw-w64-x86-64-posix mingw-w64-tools cmake ninja-build nasm pkg-config curl git zip
./tools/build-windows.sh            # into build-windows/
./tools/build-windows.sh --package  # ...and dualscreen-windows.zip
```

The first run builds SDL2, zlib, zstd and a minimal FFmpeg into
`.deps-windows/`, which takes a few minutes. H.264 is encoded by whichever of
NVENC, AMF or Media Foundation the PC has, and decoded with D3D11VA. If Wine is
installed, `cd build-windows && ctest` runs the test suite under it.

### Nintendo Switch

Needs devkitPro with the Switch toolchain. On Windows, run this from
devkitPro's MSYS2 shell, without `sudo`.

```sh
pacman -S switch-dev switch-sdl2 switch-zlib
pacman -S switch-ffmpeg            # optional: H.264 video, found automatically
pacman -S switch-curl              # optional: SteamGridDB icons for forwarders
export DEVKITPRO=/opt/devkitpro
cd switch && make
```

That produces `switch/Dualscreen.nro`. Copy it to `/switch/` on the SD card
and start it from the homebrew menu. With a DS core built in, X on a ROM in
its file browser makes the game a forwarder of its own on the HOME menu.

For a **Switch 2** (kaiHEN), `make -C switch SWITCH2=1` builds
`switch/Dualscreen-Switch2.nro` against the ouncebrew devkit unpacked into
`$DEVKITPRO`; the `switch2` job in `builds.yml` shows the exact version. It
joins games only: the Switch's built-in DS emulator draws with the Switch 1's
GPU. More in [switch/README.md](../switch/README.md).

### Android

Needs the Android SDK and NDK, and SDL2's source, which is not vendored:

```sh
git clone --depth 1 --branch release-2.30.9 \
    https://github.com/libsdl-org/SDL android/app/jni/SDL
cd android
gradle wrapper          # once, or open this directory in Android Studio
./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

The build also fetches the DS core (melonDS DS, from libretro's buildbot)
into the APK, so it can play DS games as well as join them. H.264 needs
nothing extra: it is decoded by Android's own MediaCodec. More in
[android/README.md](../android/README.md).

### Cemu, for Wii U games

Wii U games run in a fork of Cemu, `joshbirnholz/cemu-dualscreen`, branch
`claude/dualscreen-frame-link` (see [Wii U games](guide.md#wii-u-games)). It builds the way
Cemu does, with the same dependencies; the fork's own `BUILD.md` lists them
for each platform. On Linux:

```sh
git clone --recursive --branch claude/dualscreen-frame-link \
    https://github.com/joshbirnholz/cemu-dualscreen
cd cemu-dualscreen
cmake -S . -B build -DCMAKE_BUILD_TYPE=release -G Ninja
cmake --build build
```

The result is `bin/Cemu_release`. Point `wii_u_program` in `settings.json` at
it (see [Wii U games](guide.md#wii-u-games)), or let Dualscreen download the
fork's nightly for you. SteamOS has no compiler on the host, so build it
inside a container or distrobox, the same way `tools/build-steamos.sh` does
for this app. After pulling new commits, run `git submodule update --init
--recursive` as well, since Cemu's dependencies are submodules.
