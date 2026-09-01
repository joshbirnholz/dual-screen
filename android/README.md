# Dualscreen for Android

An Android app that joins a game already running on another device and becomes
one of its two screens.

It runs no emulator and holds no ROM. There is no core to install, no BIOS to
supply and nothing to dump: everything is being played on the other device, and
this app draws the screen it was given and sends its buttons and touches back.

It is the same program as the Switch companion — not a port of it, the same
`companion/source/main.cpp` and the same shared sources under `src/`, compiled
again. What is in this directory is the Android build and the handful of things
only the Java side can ask for. A player moving between a Switch, a Steam Deck
and an Android handheld should find the same menus with the same entries in the
same places.

Built and aimed at a **Logitech G Cloud** — a 1080p handheld with sticks and
buttons — but nothing here is specific to it, and a phone or tablet works as
the touch screen with no controller at all.

## What it does

See [`../switch/README.md`](../switch/README.md) for what the companion is and
how it behaves: finding hosts, joining as the top or bottom screen, the quick
menu, and the fact that everything the two devices have to agree on belongs to
the host. All of it is the same here.

## Controls

The same as everywhere else. On a device with a controller, that is the
controller; on one without, the touchscreen alone is enough to be the bottom
screen.

| | |
| --- | --- |
| Move around the menus | d-pad or left stick |
| Join, and confirm in menus | the east face button |
| Go back, or leave a menu | the south face button |
| Open this device's menu | **L3 and R3** together, the pad's **home** button, or a **two-finger tap** |
| Smooth or sharpen the screens | **R3** alone — the host applies it on both devices |
| Swap the two focus layouts | **L3** alone — likewise |
| Touch the touchscreen | tap or drag, when this device is the bottom screen |

## Building

Needs the Android SDK and NDK, and the SDL2 source. SDL2 is not vendored here —
it is large, it is not this project's code, and the version wants to be your
choice — so fetch it first:

```sh
git clone --depth 1 --branch release-2.30.9 \
    https://github.com/libsdl-org/SDL android/app/jni/SDL
```

(or set `SDL2_ANDROID_SRC` to a copy you already have). Then:

```sh
cd android
gradle wrapper          # or just open this directory in Android Studio
./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

The `gradlew` script and its jar are not committed — `gradle wrapper` writes
them, and Android Studio writes them on first open. The Gradle version they
should fetch is pinned in `gradle/wrapper/gradle-wrapper.properties`.

Both halves of SDL2 are built from that source tree: the native library, and
the `SDLActivity` Java class this app's activity extends. They have to be the
same version, which is why a prebuilt `.aar` is not used.

### H.264 video

Optional, the same way it is for the desktop build and the Switch one: the
GamePad screen carries fine as changed tiles without it, just at several
times the bandwidth. Unlike the Switch, there is no one official package for
this — build FFmpeg yourself with the NDK, with MediaCodec (Android's own
hardware decoder, reached through FFmpeg's JNI bridge) enabled:

```sh
git clone --depth 1 --branch n7.1 https://github.com/FFmpeg/FFmpeg ffmpeg
cd ffmpeg
export ANDROID_NDK_HOME=/path/to/your/ndk   # the same one Gradle uses
TOOLCHAIN=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64
API=21   # matches minSdk in app/build.gradle
# --cross-prefix has no API number -- it names ar/ranlib/strip, which the NDK
# does not version -- but --cc/--cxx do, since the compiler is what minSdk
# actually governs. A version on --cross-prefix too is the usual way this
# silently falls back to a host gcc that happens to be on PATH instead.
./configure --prefix=/path/to/ffmpeg-android/arm64-v8a \
    --target-os=android --arch=aarch64 --cpu=armv8-a \
    --sysroot=$TOOLCHAIN/sysroot \
    --cross-prefix=$TOOLCHAIN/bin/llvm- \
    --cc=$TOOLCHAIN/bin/aarch64-linux-android$API-clang \
    --cxx=$TOOLCHAIN/bin/aarch64-linux-android$API-clang++ \
    --enable-cross-compile --enable-pic --enable-jni --enable-mediacodec \
    --enable-static --disable-shared --disable-programs --disable-doc \
    --disable-avdevice --disable-avformat --disable-encoders --disable-muxers \
    --enable-decoder=h264_mediacodec --enable-hwaccel=h264_mediacodec \
    --enable-swscale
make -j$(nproc) && make install
```

Repeat for `armeabi-v7a` if that ABI matters to you —
`build.gradle`'s `abiFilters` builds both by default — with `--arch=arm
--cpu=armv7-a` and the compiler names swapped for
`armv7a-linux-androideabi$API-clang`/`clang++` (`arm`, not `aarch64`, is the
only thing that changes in the triple). Then point the app's build at
wherever `--prefix` for each ABI landed, one directory holding both:

```sh
export FFMPEG_ANDROID_DIR=/path/to/ffmpeg-android
```

Detected automatically from there: `cmake --log-level=STATUS` (or Android
Studio's own build output) says whether it was found. Nothing else
changes — a host that cannot encode video, or a companion the host is not
sending it to, still gets the tile codec exactly as before.

## Reading the log

Everything the app does goes to logcat:

```sh
adb logcat -s dualscreen
```

The same lines are also written to a file, so they can be read afterwards
without a computer attached:

```
/sdcard/Android/data/com.dualscreen.companion/files/dualscreen-companion.log
```

A fresh one per run, flushed a line at a time, so it survives the app being
killed rather than exiting.

## Why the app asks for Wi-Fi permissions

`INTERNET` is the obvious one. The other two are what make finding a host work
at all:

- **`CHANGE_WIFI_MULTICAST_STATE`** lets the app hold a *multicast lock*.
  Without one, the Wi-Fi chipset drops broadcast and multicast packets before
  they ever reach the app, to save power. Hosts announce themselves by
  broadcast, so the symptom is not a slow search or an error — the list stays
  empty for ever, on a network where everything else is working. If hosts are
  never found, this lock is the first thing to check in the log.
- **`ACCESS_WIFI_STATE`** is what taking either lock requires. The app also
  takes a Wi-Fi lock in low-latency mode, because Wi-Fi power saving parks the
  radio between packets and a video stream feels that as stutter.

Neither reveals anything about the network, and the app asks for no location
permission, which is what scanning for nearby networks would need. It never
scans.

## Being taken off screen

Android can take the drawing surface away while the app keeps running — the
task switcher, a notification pulled down, the screen locked. The app stops
drawing for as long as that lasts and keeps the session polled, so glancing at
something else and coming back does not cost the connection.

Coming back it rebuilds every GL object it owns rather than trusting the ones
it had. The context is not promised to survive being backgrounded, and a
texture that did not survive is still a valid-looking number — the failure
would be a screen of garbage rather than an error.

## The Standard layout on a 1080p handheld

Every device drops the margin around the screens once its display is small
enough — 1280x800 or under, which is what a Steam Deck and a handheld Switch
both are — and fills the height with the console's own screens instead, with
the artwork cropped around them.

A G Cloud is a 7" screen, physically the same size as those, but it is
1920x1080, so by that rule alone it would count as a large display and keep
the margin. Counting pixels is a stand-in for how big the screen really is,
and on this device the two part company.

So there is a second rule that does not count pixels at all: **a device
holding the touch screen fills its panel with it**, whatever its resolution.
Being the screen that is touched is the thing that decides it — a companion
showing the touch screen is a thing being held and prodded, and wants that
screen as large as the panel allows. Joined as the top screen, or hosting,
the pixel rule applies as before.

If the margin should also go when this device is the *top* screen, the
threshold is `kSmallDisplayWidth`/`kSmallDisplayHeight` at the top of
`src/layout/layout.cpp`, and raising it changes every device at once.

## What is here

| | |
| --- | --- |
| `app/CMakeLists.txt` | the native build — the shared sources plus SDL2 |
| `app/build.gradle` | the Android build, and where SDL2 is expected |
| `app/src/main/java/.../DualscreenActivity.java` | the Wi-Fi locks and keeping the screen on |
| `app/src/main/AndroidManifest.xml` | permissions, landscape, launcher entry |

The list of sources the app is built from lives in
[`../cmake/companion_sources.cmake`](../cmake/companion_sources.cmake), read by
this build and by the desktop one, so a file added to the client cannot go
missing from either.
