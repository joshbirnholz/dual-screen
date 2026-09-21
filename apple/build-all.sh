#!/usr/bin/env bash
#
# Generates and builds the companion for every Apple platform at once, and puts
# the projects in one Xcode workspace.
#
# This is as close to "one multiplatform app" as CMake can get, and the gap is
# worth understanding rather than working around. CMake bakes the target
# platform into a project at configure time -- CMAKE_SYSTEM_NAME is one value
# for one project -- so three platforms mean three configures and three
# projects. Xcode itself can do better: a single target with SUPPORTED_PLATFORMS
# covering several destinations, which is what its own multiplatform template
# makes. CMake cannot express that, and hand-writing the project instead would
# mean a second copy of the source list every other client reads from one
# place. See apple/README.md.
#
# What it is *not* short of: the three bundles already share one bundle
# identifier, which is what Apple's Universal Purchase wants. One product, one
# page, one purchase -- separate binaries underneath, which is how every
# multiplatform Apple app works anyway.
#
# Nothing here has been run on a Mac. See "What has actually been verified".

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "${here}/.." && pwd)"
out="${root}/build-apple"
team=""
device=0
generator="Ninja"
run=""
sdl_prefix=""
platforms=()

usage() {
    cat <<'USAGE'
usage: apple/build-all.sh [options] [macos] [ios] [tvos]

  Named platforms, or all three if none are named.

  --xcode           generate Xcode projects and a workspace, instead of
                    building with Ninja. Needed for a debugger, for real
                    hardware, and for anything to do with submission.
                    Use --sdl-prefix with it.
  --sdl-prefix <p>  an SDL2 already built and installed for this platform,
                    instead of building one here. Strongly wanted with
                    --xcode: SDL's own feature checks are where the Xcode
                    generator breaks, and a prebuilt SDL has none to run.
                    See apple/README.md for the one command that makes it.
  --run <platform>  after building, install and launch on a booted simulator
                    (ios or tvos). Needs a simulator running already:
                    open -a Simulator
  --device          build iOS/tvOS for real hardware rather than a simulator
                    (needs --team, and --xcode to deploy)
  --team <id>       the Apple development team to sign with
  --out <dir>       where to put the builds (default: build-apple/)
  -h, --help        this

  Ninja by default, because it is what works: measured on CMake 4.4.3 with
  Xcode 27, the same CMakeLists configures cleanly under Ninja and fails every
  feature check under the Xcode generator. A Ninja build still produces a real
  .app, and a simulator needs no signature -- see --run.
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --device) device=1; shift ;;
        --xcode) generator="Xcode"; shift ;;
        --sdl-prefix) sdl_prefix="${2:-}"; shift 2 ;;
        --run) run="${2:-}"; shift 2 ;;
        --team) team="${2:-}"; shift 2 ;;
        --out) out="${2:-}"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        macos|ios|tvos) platforms+=("$1"); shift ;;
        *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ ${#platforms[@]} -eq 0 ]]; then
    if [[ -n "${run}" ]]; then
        # Asking to run one platform is asking to build that one, not all of
        # them and then run one.
        platforms=("${run}")
    else
        platforms=(macos ios tvos)
    fi
fi

if [[ ${device} -eq 1 && "${generator}" != "Xcode" ]]; then
    echo "note: --device builds for real hardware, which has to be signed and" >&2
    echo "      deployed through Xcode. Add --xcode, or drop --device and use" >&2
    echo "      a simulator." >&2
fi

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "This builds Apple applications and needs a Mac with Xcode." >&2
    exit 1
fi

mkdir -p "${out}"

configure_one() {
    local platform="$1"
    local dir="${out}/${platform}"
    local -a flags=(-S "${here}" -B "${dir}" -G "${generator}")

    case "${platform}" in
        macos)
            # No CMAKE_SYSTEM_NAME: unset means Darwin, which is a Mac.
            ;;
        ios)
            flags+=(-DCMAKE_SYSTEM_NAME=iOS)
            # Ninja builds one architecture and has no notion of picking one
            # per destination, so it has to be told which.
            if [[ "${generator}" != "Xcode" ]]; then
                flags+=(-DCMAKE_OSX_ARCHITECTURES=arm64)
            fi
            # Written out rather than as `cond && flags+=(...)`: under `set -e`
            # a false condition there makes the list return non-zero, which is
            # enough to end the script -- silently, and only when building for
            # a device, which is the run you would least want to lose.
            if [[ ${device} -eq 0 ]]; then
                flags+=(-DCMAKE_OSX_SYSROOT=iphonesimulator)
            fi
            ;;
        tvos)
            flags+=(-DCMAKE_SYSTEM_NAME=tvOS)
            if [[ "${generator}" != "Xcode" ]]; then
                flags+=(-DCMAKE_OSX_ARCHITECTURES=arm64)
            fi
            if [[ ${device} -eq 0 ]]; then
                flags+=(-DCMAKE_OSX_SYSROOT=appletvsimulator)
            fi
            ;;
    esac
    if [[ -n "${team}" ]]; then
        flags+=("-DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=${team}")
    fi
    # An SDL that is already built, which is what makes the Xcode generator
    # usable: every configure failure seen on iOS so far was inside SDL's own
    # feature checks. See DUALSCREEN_SDL2_PREFIX in CMakeLists.txt.
    if [[ -n "${sdl_prefix}" && "${platform}" != "macos" ]]; then
        flags+=("-DDUALSCREEN_SDL2_PREFIX=${sdl_prefix}")
    fi

    echo "=== configuring ${platform} (${generator})"
    cmake "${flags[@]}"
    echo "=== building ${platform}"
    cmake --build "${dir}" --config RelWithDebInfo
}

# Install and launch on whichever simulator is booted. A simulator app needs no
# signature, which is why this works without a developer account at all.
launch_on_simulator() {
    local platform="$1"
    local bundle="${out}/${platform}/Dualscreen Companion.app"
    if [[ ! -d "${bundle}" ]]; then
        echo "no ${bundle} to install -- was ${platform} built?" >&2
        return 1
    fi
    if ! xcrun simctl list devices booted | grep -q "(Booted)"; then
        echo "no simulator is booted. Start one first:  open -a Simulator" >&2
        return 1
    fi
    echo "=== installing ${platform} on the booted simulator"
    xcrun simctl install booted "${bundle}"
    echo "=== launching (ctrl-c to stop; the app's log follows)"
    xcrun simctl launch --console booted com.joshbirnholz.dualscreen.companion
}

for platform in "${platforms[@]}"; do
    configure_one "${platform}"
done

if [[ -n "${run}" ]]; then
    launch_on_simulator "${run}"
    exit $?
fi

# The workspace is an Xcode thing, so there is only one when there are Xcode
# projects to put in it.
if [[ "${generator}" != "Xcode" ]]; then
    echo
    echo "Built: ${platforms[*]}  (in ${out})"
    echo "Run:   apple/build-all.sh --run ios      # with a simulator open"
    exit 0
fi

# One window with every platform's scheme in it, which is the part that
# actually feels like one app. A workspace is a list of projects and nothing
# else -- no build settings, nothing to drift.
workspace="${out}/Dualscreen.xcworkspace"
mkdir -p "${workspace}"
{
    printf '<?xml version="1.0" encoding="UTF-8"?>\n'
    printf '<Workspace version = "1.0">\n'
    for platform in "${platforms[@]}"; do
        # Relative to the workspace, which sits beside the project directories.
        printf '   <FileRef location = "group:%s/dualscreen_companion.xcodeproj"></FileRef>\n' \
               "${platform}"
    done
    printf '</Workspace>\n'
} > "${workspace}/contents.xcworkspacedata"

echo
echo "Built: ${platforms[*]}"
echo "Open:  open \"${workspace}\""
