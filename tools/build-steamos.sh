#!/usr/bin/env bash
#
# Builds dualscreen for SteamOS.
#
# SteamOS has a read-only root filesystem and no compiler, so the build happens
# inside a container. The container image is deliberately older than SteamOS
# (Debian 12), because a binary built against an older glibc runs on a newer
# one but not the other way round -- so the result runs directly on the SteamOS
# host, in Game Mode, with no container at runtime and nothing to redo after a
# SteamOS update.
#
# Run it from anywhere in the repo:
#
#     ./tools/build-steamos.sh
#
# Pass --rebuild to start from a clean build directory, or --shell to drop into
# the build container instead of building.

set -euo pipefail

CONTAINER_NAME="dualscreen-build"
CONTAINER_IMAGE="debian:12"
REPO_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${REPO_DIR}/build"

say()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m==>\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m==>\033[0m %s\n' "$*" >&2; exit 1; }

REBUILD=0
SHELL_ONLY=0
for argument in "$@"; do
    case "$argument" in
        --rebuild) REBUILD=1 ;;
        --shell)   SHELL_ONLY=1 ;;
        -h|--help) sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unrecognised argument: $argument" ;;
    esac
done

# The commands that actually do the build. Run inside the container, or
# directly if we are already somewhere with a compiler.
build_here() {
    if [ "$REBUILD" -eq 1 ]; then
        rm -rf "$BUILD_DIR"
    fi
    cmake -S "$REPO_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build "$BUILD_DIR" -j"$(nproc)"
}

install_dependencies() {
    sudo apt-get update -qq
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
        build-essential cmake pkg-config libsdl2-dev nlohmann-json3-dev \
        libavcodec-dev libavutil-dev libswscale-dev
}

# --- already inside a container (or a normal Linux dev box)? just build -------
if [ -f /run/.containerenv ] || [ -f /.dockerenv ] || [ "${DUALSCREEN_IN_CONTAINER:-0}" = "1" ]; then
    say "building in place"
    command -v cmake >/dev/null 2>&1 || install_dependencies
    build_here
    exit 0
fi

if ! grep -qi 'steamos\|holo' /etc/os-release 2>/dev/null; then
    if command -v cmake >/dev/null 2>&1 && command -v g++ >/dev/null 2>&1; then
        say "not SteamOS and a compiler is present; building directly"
        build_here
        exit 0
    fi
fi

# --- on SteamOS: build in a container ----------------------------------------
if ! command -v distrobox >/dev/null 2>&1; then
    die "distrobox was not found.

SteamOS's root filesystem is read-only, so the build needs a container.
distrobox ships with recent SteamOS releases; if yours does not have it,
install it into your home directory without touching the root filesystem:

    curl -s https://raw.githubusercontent.com/89luca89/distrobox/main/install | sh -s -- --prefix ~/.local

then add ~/.local/bin to your PATH and run this script again.

Do NOT use 'sudo steamos-readonly disable' for this: packages installed that
way are wiped by every SteamOS update, so you would be redoing it forever."
fi

if ! distrobox list 2>/dev/null | grep -q "[[:space:]]${CONTAINER_NAME}[[:space:]]"; then
    say "creating the ${CONTAINER_NAME} container (${CONTAINER_IMAGE}) -- first run only"
    distrobox create --name "$CONTAINER_NAME" --image "$CONTAINER_IMAGE" --yes
    say "installing build dependencies"
    distrobox enter "$CONTAINER_NAME" -- bash -lc "$(declare -f install_dependencies); install_dependencies"
else
    say "reusing the existing ${CONTAINER_NAME} container"
fi

if [ "$SHELL_ONLY" -eq 1 ]; then
    exec distrobox enter "$CONTAINER_NAME"
fi

say "building"
distrobox enter "$CONTAINER_NAME" -- bash -lc "
    set -euo pipefail
    export DUALSCREEN_IN_CONTAINER=1
    REPO_DIR='${REPO_DIR}' BUILD_DIR='${BUILD_DIR}' REBUILD='${REBUILD}'
    $(declare -f build_here)
    build_here
"

BINARY="${BUILD_DIR}/dualscreen"
[ -x "$BINARY" ] || die "the build finished but ${BINARY} is missing"

say "built ${BINARY}"

# The whole point of the older image is that the binary runs on the host, so
# check that here rather than taking it on faith.
if "$BINARY" --list-layouts >/dev/null 2>&1; then
    say "it runs on the SteamOS host -- add it to Steam as a non-Steam game:"
    printf '\n    %s --game "/path/to/rom.nds"\n\n' "$BINARY"
else
    warn "the binary did not run on the host. Its unmet requirements:"
    ldd "$BINARY" 2>&1 | grep -i 'not found' || true
    warn "'ldd --version' on the host vs. in the container will show a glibc mismatch;
report it as an issue with both versions."
fi
