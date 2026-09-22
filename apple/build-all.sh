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

# The architectures a prebuilt SDL actually contains.
#
# This is the difference between a build that links and one that does not. Under
# the Xcode generator nothing here sets ARCHS, on the theory that Xcode picks an
# architecture per destination -- which it does, when a person builds with a
# destination selected. This script has none: it drives xcodebuild through
# `cmake --build`, so ONLY_ACTIVE_ARCH has no active architecture to resolve and
# Xcode says so and builds every architecture the SDK supports. For a simulator
# SDK that is arm64 *and* x86_64, while the SDL built by the command in
# apple/README.md is arm64 alone. The arm64 slice links; the x86_64 slice finds
# no SDL in the archive, ignores all 200 of its members one warning at a time,
# and fails on undefined _main and _OBJC_CLASS_$_SDLUIKitDelegate -- which reads
# as a mistake in the scene life cycle file rather than as an architecture that
# was never going to work.
#
# So ask the library. lipo reports what is in an archive, and building for
# exactly those architectures is right whichever way SDL was built: arm64 on
# Apple silicon, x86_64 on an Intel Mac, both if someone made a fat one.
sdl_architectures() {
    local prefix="$1"
    local lib archs
    for lib in "${prefix}/lib/libSDL2.a" "${prefix}/lib/libSDL2.dylib"; do
        if [[ -e "${lib}" ]] && archs="$(lipo -archs "${lib}" 2>/dev/null)"; then
            # Semicolons, because CMAKE_OSX_ARCHITECTURES is a CMake list and
            # lipo separates with spaces. "x86_64 arm64" passed through as
            # written is one architecture with a space in its name, and the
            # error that follows names neither of them.
            printf '%s' "${archs// /;}"
            return 0
        fi
    done
    return 1
}

# Where a platform's project goes.
#
# A device build and a simulator build are different SDKs, and CMake bakes the
# SDK into a project at configure time the same way it bakes in the platform.
# Sharing one directory between them means the second configure lands on the
# first one's cache: at best a reconfigure, at worst a project whose SDKROOT
# says one thing and whose cache says another. Xcode's symptom for that is
# "Mismatched platform" in the destination picker -- a simulator project
# offering no real Apple TV to build for, with nothing on screen to say why.
#
# So they get a directory each and can both exist. That is the first of the two
# answers in apple/README.md under "The device-and-simulator catch", and it
# applies to the SDL prefix too: one prefix is built for one SDK, so a device
# build needs a device SDL.
build_dir() {
    local platform="$1"
    if [[ ${device} -eq 1 && "${platform}" != "macos" ]]; then
        printf '%s/%s-device' "${out}" "${platform}"
    else
        printf '%s/%s' "${out}" "${platform}"
    fi
}

configure_one() {
    local platform="$1"
    local dir
    dir="$(build_dir "${platform}")"
    local -a flags=(-S "${here}" -B "${dir}" -G "${generator}")

    case "${platform}" in
        macos)
            # No CMAKE_SYSTEM_NAME: unset means Darwin, which is a Mac.
            ;;
        ios)
            flags+=(-DCMAKE_SYSTEM_NAME=iOS)
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
            if [[ ${device} -eq 0 ]]; then
                flags+=(-DCMAKE_OSX_SYSROOT=appletvsimulator)
            fi
            ;;
    esac

    # Which architectures to build, for both generators and either way of
    # building. Ninja produces one architecture and has to be told which;
    # xcodebuild without a destination produces all of them and has to be told
    # not to. The prebuilt SDL answers both -- see sdl_architectures.
    if [[ "${platform}" != "macos" ]]; then
        local archs=""
        if [[ -n "${sdl_prefix}" ]]; then
            archs="$(sdl_architectures "${sdl_prefix}" || true)"
            if [[ -z "${archs}" ]]; then
                echo "note: no SDL2 library under ${sdl_prefix}/lib to read an" >&2
                echo "      architecture from; building arm64." >&2
            fi
        fi
        # arm64 when there is nothing to ask: every Apple device is arm64, and
        # so is every simulator on an Apple silicon Mac.
        flags+=("-DCMAKE_OSX_ARCHITECTURES=${archs:-arm64}")
    fi

    if [[ -n "${team}" ]]; then
        flags+=("-DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=${team}")
    fi
    # An SDL that is already built, which is what makes the Xcode generator
    # usable: every configure failure seen on iOS so far was inside SDL's own
    # feature checks. See DUALSCREEN_SDL2_PREFIX in CMakeLists.txt.
    if [[ -n "${sdl_prefix}" && "${platform}" != "macos" ]]; then
        flags+=("-DDUALSCREEN_SDL2_PREFIX=${sdl_prefix}")
    fi

    # Which SDK, said out loud. A prefix built for the other one fails in the
    # linker with nothing to point at, so the line above the failure should at
    # least say what this build was for.
    if [[ "${platform}" != "macos" ]]; then
        local which_sdk="a simulator"
        if [[ ${device} -eq 1 ]]; then which_sdk="real hardware"; fi
        echo "=== configuring ${platform} for ${which_sdk} (${generator})"
        if [[ -n "${sdl_prefix}" ]]; then
            echo "    SDL: ${sdl_prefix} -- which must itself be built for ${which_sdk}"
        fi
    else
        echo "=== configuring ${platform} (${generator})"
    fi
    cmake "${flags[@]}"
    echo "=== building ${platform}"
    cmake --build "${dir}" --config RelWithDebInfo
}

# Where a build put its bundle. Ninja is one configuration and puts it in the
# build directory; Xcode is several and puts each in a directory of its own.
# Looked for rather than worked out, because --run has to find whichever the
# last build made and the generator is not always the one this run was given.
find_bundle() {
    local platform="$1"
    local dir
    dir="$(build_dir "${platform}")"
    local candidate
    for candidate in "${dir}/Dualscreen Companion.app" \
                     "${dir}/RelWithDebInfo/Dualscreen Companion.app" \
                     "${dir}/Debug/Dualscreen Companion.app" \
                     "${dir}/Release/Dualscreen Companion.app"; do
        if [[ -d "${candidate}" ]]; then
            printf '%s' "${candidate}"
            return 0
        fi
    done
    return 1
}

# Install and launch on whichever simulator is booted. A simulator app needs no
# signature, which is why this works without a developer account at all.
launch_on_simulator() {
    local platform="$1"
    local bundle
    if ! bundle="$(find_bundle "${platform}")"; then
        echo "no Dualscreen Companion.app under $(build_dir "${platform}") -- was ${platform} built?" >&2
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
               "$(basename "$(build_dir "${platform}")")"
    done
    printf '</Workspace>\n'
} > "${workspace}/contents.xcworkspacedata"

echo
echo "Built: ${platforms[*]}"
echo "Open:  open \"${workspace}\""
