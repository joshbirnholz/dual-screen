# Dualscreen for Android

An Android app that joins a game already running on another device and becomes
one of its two screens -- and, like the Switch build with a core in it, plays
DS games itself as well.

Joining needs nothing: no core, no BIOS, no ROM. Everything is being played on
the other device, and this app draws the screen it was given and sends its
buttons and touches back. Playing here needs only the ROM; the DS core rides
in the APK (see **Playing DS games here** below).

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

## Playing DS games here

The opening screen has **Load a game** and **Settings**, as a Switch's does
when it is built with a core, and the app plays DS games with everything the
desktop app has for them: the same file browser, layouts and quick menu,
save states, Save Sync, **Resume from Save State**, Data Management, and a
second device joining as the other screen. It is the desktop app's own code,
compiled again -- not a port of it.

The emulator is **melonDS DS**, the libretro core the desktop app plays DS
games with. `app/build.gradle` fetches it from libretro's buildbot the first
time the app is built, into `app/jni/melonds-ds/<abi>/` (ignored by git), and
packages it beside the app's own library; the app loads it when a game starts.
To use a build of your own instead, set `DS_CORE_ANDROID_DIR` to a folder laid
out the same way (`<abi>/libmelondsds_libretro_android.so`). `-PnoDsCore`
builds an APK without one: then the app is the companion alone and says so in
its log, and Load a game is not offered.

No BIOS or firmware is needed -- melonDS DS falls back to its own -- though
`bios7.bin`, `bios9.bin` and `firmware.bin` are used if they are put in
`dualscreen/system/` (below).

**Load a game** first asks for access to the device's files: games live in
shared storage, which from Android 11 an app can read only with **access to
all files**, a switch in the system's own settings. The app opens that page
the first time it is needed, never at start-up, and goes on to the file
browser by itself once it is allowed. The browser opens on `roms/nds` (or
`roms/NDS`, `roms/ds`, `ROMs/nds`, `RetroArch/roms/nds`, `roms`, `ROMs`,
`Download`, whichever is there first) in shared storage, and after that
wherever a game was last chosen from.

**+** (Main Menu) in the browser goes back to the opening screen. Quitting a
game from its quick menu closes the app, as it does on a Switch.

The menu and the game take turns in one process: choosing Load a game closes
the menu's window and the game opens its own, and leaving the game for the
main menu does the reverse. (A Switch starts the program again between the
two; an Android app has nobody to ask to do that.)

### Where it keeps things

Everything is in the app's own folder in shared storage, where a file manager
or a USB cable can reach it, and which is removed with the app:

```
/sdcard/Android/data/com.dualscreen.companion/files/
    Dualscreen.log
    dualscreen/
        settings.json      the app's settings
        games/             per-game options and layouts
        saves/             cartridge saves (.srm, as RetroArch's), save states
        system/            BIOS and firmware, if you want melonDS DS to use them
```

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

## Motion

A Wii U GamePad has a gyroscope and an accelerometer, and a great many of its
games steer, aim or look around with them -- Splatoon, ZombiU, Mario Kart 8,
Wind Waker HD's telescope. Joined to a Wii U host, this device stands in for
that GamePad, so how it is being held is sent along with its buttons.

What crosses the wire is raw: rotation in radians per second and acceleration
in metres per second squared, exactly as the sensors report them, plus the
moment each reading was taken. Nothing on this side turns that into an
orientation. Cemu already does, with sensor fusion tuned to what a real
GamePad reports, and a second answer to the same question computed here would
only be a different one.

Only on a console that reads it. The host says whether it does, and until it
says so no sensor is switched on at all -- a gyroscope left running is
battery spent for nothing on a handheld. So a DS or 3DS session costs this
nothing, and a Wii U session starts the sensors when it connects and stops
them when it leaves.

The log says what it found, once, when a Wii U host asks:

```
motion: the host reads it; 1 of 1 connected controllers report it
motion: this device is reporting how it is being held
```

`0 of 1` is the line to look for when aiming does nothing. It means SDL cannot
see any sensors on the pad, which is almost never the pad's fault -- see the
device-specific note below.

### On this device

Two places it can come from, tried in that order:

- **A controller with sensors**, if one is attached and SDL can see them.
- **The device's own gyroscope and accelerometer**, which is what a phone or
  tablet has and what a G Cloud does not. These are not attached to any
  controller, so SDL reports them as standalone sensors and this app opens
  them separately.

A handheld with buttons but no sensors of its own and no sensor-bearing pad --
a G Cloud is exactly that -- has no motion to send, and says so in the log.
Games that need it are then played the way they are with a Pro Controller: not
at all, or with whatever the game offers instead.

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

Nothing to install. H.264 -- what a host sends a Wii U GamePad's screen as,
and any screen it judges too busy for tiles -- is decoded by Android's own
MediaCodec, reached through the NDK (`libmediandk`, part of every Android
since 5.0, this app's floor). That is the device's hardware decoder wherever
it has one, which is every Android handheld; the app says which in its log:

```
video decoder: MediaCodec c2.qti.avc.decoder, configured for 1280x720
video decoder: pictures arrive 1280x720, stride 1280, 720 rows to a plane, colour format 0x7fa30c04 (NV12)
```

Frames come back as planes (NV12, or I420 from a software decoder) and are
turned into colour while they are drawn, not on the CPU. See
`src/video/mediacodec_decoder.hpp`.

Before this, H.264 here meant cross-compiling FFmpeg with the NDK, which needs
a Unix shell. FFmpeg is still honoured if a self-built one is pointed at with
`FFMPEG_ANDROID_DIR` (laid out `<dir>/<abi>/{include,lib}`), but MediaCodec is
tried first and FFmpeg's own `h264_mediacodec` would only reach the same
hardware the long way round.

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
| `app/src/main/java/.../DualscreenActivity.java` | the Wi-Fi locks, keeping the screen on, and asking for access to files |
| `app/src/main/AndroidManifest.xml` | permissions, landscape, launcher entry |

The list of sources the app is built from lives in
[`../cmake/companion_sources.cmake`](../cmake/companion_sources.cmake), read by
this build and by the desktop one, so a file added to the client cannot go
missing from either -- the companion's, and the desktop app's beyond it that
playing DS games here needs.
