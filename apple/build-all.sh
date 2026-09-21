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
platforms=()

usage() {
    cat <<'USAGE'
usage: apple/build-all.sh [options] [macos] [ios] [tvos]

  Named platforms, or all three if none are named.

  --device          build iOS/tvOS for real hardware rather than a simulator
                    (needs --team, or a team set in Xcode)
  --team <id>       the Apple development team to sign with
  --out <dir>       where to put the projects (default: build-apple/)
  -h, --help        this

  Afterwards:  open build-apple/Dualscreen.xcworkspace
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --device) device=1; shift ;;
        --team) team="${2:-}"; shift 2 ;;
        --out) out="${2:-}"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        macos|ios|tvos) platforms+=("$1"); shift ;;
        *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ ${#platforms[@]} -eq 0 ]]; then
    platforms=(macos ios tvos)
fi

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "This builds Apple applications and needs a Mac with Xcode." >&2
    exit 1
fi

mkdir -p "${out}"

configure_one() {
    local platform="$1"
    local dir="${out}/${platform}"
    local -a flags=(-S "${here}" -B "${dir}" -G Xcode)

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
    if [[ -n "${team}" ]]; then
        flags+=("-DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=${team}")
    fi

    echo "=== configuring ${platform}"
    cmake "${flags[@]}"
    echo "=== building ${platform}"
    cmake --build "${dir}" --config RelWithDebInfo
}

for platform in "${platforms[@]}"; do
    configure_one "${platform}"
done

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
