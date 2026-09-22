#!/usr/bin/env bash
#
# What apple/build-all.sh asks cmake for: the architectures, and where the
# project goes.
#
# Both have cost a build on a Mac, and neither is visible from here.
#
# An Apple TV build failed at the link, with undefined _main and
# _OBJC_CLASS_$_SDLUIKitDelegate, after 200 warnings about ignoring every member
# of libSDL2.a for having architecture arm64 when x86_64 was required. Nothing
# was wrong with the code: the script left ARCHS alone under the Xcode
# generator, on the theory that Xcode picks per destination -- true when a
# person builds with one selected, and not true of the `cmake --build` this
# script runs, which has no destination and so builds every architecture the
# simulator SDK has. The prebuilt SDL had one of them.
#
# The mistake is invisible on Linux and expensive on a Mac, which is what this
# is for. Nothing Apple is installed here, so uname, lipo and cmake are stubs:
# uname to get past the Darwin check, lipo to answer for an SDL that is not
# there, and cmake to record the arguments instead of configuring anything.
#
# Usage: run_apple_build_test.sh <build-all.sh> <work dir>

set -uo pipefail

BUILD_ALL="$1"
WORK_DIR="$2"

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR/bin" "$WORK_DIR/sdl/lib"

# The stubs. lipo reports whatever LIPO_ARCHS says, so one fake SDL can stand in
# for an arm64 one, a fat one, or a library that cannot be read at all.
cat > "$WORK_DIR/bin/uname" <<'STUB'
#!/usr/bin/env bash
[ "${1:-}" = "-s" ] && { echo Darwin; exit 0; }
exec /usr/bin/uname "$@"
STUB
cat > "$WORK_DIR/bin/lipo" <<'STUB'
#!/usr/bin/env bash
[ -n "${LIPO_ARCHS:-}" ] || exit 1
echo "${LIPO_ARCHS}"
STUB
cat > "$WORK_DIR/bin/cmake" <<'STUB'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "${ARGS_LOG}"
# Real cmake makes the directory it is given, and whether two builds can sit
# side by side is one of the things being checked here.
prev=""
for arg in "$@"; do
    [ "$prev" = "-B" ] && mkdir -p "$arg"
    prev="$arg"
done
exit 0
STUB
chmod +x "$WORK_DIR/bin/uname" "$WORK_DIR/bin/lipo" "$WORK_DIR/bin/cmake"
touch "$WORK_DIR/sdl/lib/libSDL2.a"

export PATH="$WORK_DIR/bin:$PATH"

failures=0

# Runs the script and leaves its cmake arguments in $WORK_DIR/args.
run_build_all() {
    export ARGS_LOG="$WORK_DIR/args"
    : > "$ARGS_LOG"
    "$BUILD_ALL" --out "$WORK_DIR/out" "$@" > "$WORK_DIR/output" 2>&1
}

# $1 what is being checked, $2 the text the configure line must contain.
expect_flag() {
    if grep -q -- "$2" "$WORK_DIR/args"; then
        echo "ok - $1"
    else
        echo "NOT OK - $1"
        echo "    expected to find: $2"
        echo "    cmake was called with:"
        sed 's/^/      /' "$WORK_DIR/args"
        failures=$((failures + 1))
    fi
}

# $1 what is being checked, $2 the text the configure line must not contain.
expect_no_flag() {
    if grep -q -- "$2" "$WORK_DIR/args"; then
        echo "NOT OK - $1"
        echo "    did not expect: $2"
        sed 's/^/      /' "$WORK_DIR/args"
        failures=$((failures + 1))
    else
        echo "ok - $1"
    fi
}

# The failure this was written for: Xcode generator, prebuilt arm64 SDL. Before
# the fix this asked for no architecture at all and xcodebuild picked both.
LIPO_ARCHS=arm64 run_build_all --xcode --sdl-prefix "$WORK_DIR/sdl" tvos
expect_flag "an arm64 SDL builds an arm64 Apple TV app" \
            "-DCMAKE_OSX_ARCHITECTURES=arm64"

LIPO_ARCHS=arm64 run_build_all --xcode --sdl-prefix "$WORK_DIR/sdl" ios
expect_flag "an arm64 SDL builds an arm64 iOS app" \
            "-DCMAKE_OSX_ARCHITECTURES=arm64"

# A fat SDL, which is the case that says whether the separator is right: CMake
# reads this as a list, and lipo writes it with spaces.
LIPO_ARCHS="x86_64 arm64" run_build_all --xcode --sdl-prefix "$WORK_DIR/sdl" tvos
expect_flag "a fat SDL builds both, as a CMake list" \
            "-DCMAKE_OSX_ARCHITECTURES=x86_64;arm64"

# No SDL to ask, which is how the Ninja builds have always run.
run_build_all ios
expect_flag "arm64 when there is no SDL to ask" \
            "-DCMAKE_OSX_ARCHITECTURES=arm64"

# A prefix with a library lipo cannot read. Falling back is right; doing it
# quietly is not, because the app would be built for an architecture the SDL
# may not have and the link error names neither.
LIPO_ARCHS="" run_build_all --xcode --sdl-prefix "$WORK_DIR/sdl" tvos
expect_flag "an unreadable SDL still builds arm64" \
            "-DCMAKE_OSX_ARCHITECTURES=arm64"
if grep -q "architecture from" "$WORK_DIR/output"; then
    echo "ok - and says that it could not read one"
else
    echo "NOT OK - an unreadable SDL should say so"
    sed 's/^/      /' "$WORK_DIR/output"
    failures=$((failures + 1))
fi

# A Mac is not pinned: it has no prebuilt SDL here, it builds with Ninja for the
# machine it is on, and a universal binary is a reasonable thing to want.
run_build_all macos
expect_no_flag "a Mac build is left to decide for itself" \
               "-DCMAKE_OSX_ARCHITECTURES"

# A device build and a simulator build are different SDKs, and a project is
# configured for one of them. Sharing a directory means the second configure
# lands on the first one's cache, and Xcode's symptom is "Mismatched platform"
# in the destination picker: a simulator project offering no Apple TV to build
# for, saying nothing about why.
LIPO_ARCHS=arm64 run_build_all --xcode --sdl-prefix "$WORK_DIR/sdl" tvos
expect_flag "a simulator build configures build-apple/tvos" \
            "-B $WORK_DIR/out/tvos "
LIPO_ARCHS=arm64 run_build_all --xcode --device --team TEAM \
                               --sdl-prefix "$WORK_DIR/sdl" tvos
expect_flag "a device build gets a directory of its own" \
            "-B $WORK_DIR/out/tvos-device "
expect_flag "and is signed" "-DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=TEAM"
# No sysroot on a device build: CMAKE_SYSTEM_NAME=tvOS already defaults to the
# device SDK, and naming the simulator one is how you get the project that
# cannot see an Apple TV.
expect_no_flag "and is not pinned to the simulator SDK" \
               "appletvsimulator"

# The workspace has to point at whichever directories this run made, or opening
# it gives you the previous build's projects.
if grep -q "tvos-device/dualscreen_companion.xcodeproj" \
        "$WORK_DIR/out/Dualscreen.xcworkspace/contents.xcworkspacedata"; then
    echo "ok - the workspace opens the device project"
else
    echo "NOT OK - the workspace should list the device project"
    sed 's/^/      /' "$WORK_DIR/out/Dualscreen.xcworkspace/contents.xcworkspacedata"
    failures=$((failures + 1))
fi

# Both SDKs at once, which is the arrangement the README recommends and the
# reason the directories are separate at all.
LIPO_ARCHS=arm64 run_build_all --xcode --sdl-prefix "$WORK_DIR/sdl" tvos
if [ -d "$WORK_DIR/out/tvos" ] && [ -d "$WORK_DIR/out/tvos-device" ]; then
    echo "ok - a simulator build and a device build can both exist"
else
    echo "NOT OK - one build directory replaced the other"
    ls "$WORK_DIR/out" | sed 's/^/      /'
    failures=$((failures + 1))
fi

if [ "$failures" -ne 0 ]; then
    echo "$failures check(s) failed"
    exit 1
fi
echo "ok - build-all.sh asks for the right architectures, in the right place"
