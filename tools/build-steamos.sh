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
# the build container instead of building. --rebuild does not redo the FFmpeg
# build below (that is cached separately, and takes real time); delete
# .ffmpeg-steamos/ by hand for that.

set -euo pipefail

CONTAINER_NAME="dualscreen-build"
CONTAINER_IMAGE="debian:12"
REPO_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${REPO_DIR}/build"

# A from-source, deliberately minimal FFmpeg: H.264 decode and VAAPI-encode
# only, nothing else. Debian's own libavcodec-dev links dozens of codecs
# this project has no use for -- AV1 (dav1d, rav1e, SVT-AV1, aom), HEVC
# (x265), Theora, and JPEG XL, which drags in librsvg, libcairo and glib for
# reasons that have nothing to do with video -- every one of which the
# SteamOS host would then also need, and bundling private copies of glib and
# cairo alongside whatever the Deck's desktop is already running is the kind
# of thing that usually works and occasionally does not. A stripped build
# has none of that: three small libraries whose own dependencies are just
# libva/libdrm/libX11, the same class of "any Linux desktop with GPU
# acceleration already has this" assumption libGL already gets elsewhere in
# this build (see CMakeLists.txt) -- so only avcodec/avutil/swscale
# themselves need bundling, not everything they in turn pull in.
#
# Cached under the repo root rather than $BUILD_DIR so a plain --rebuild (or
# CI wiping build/ between runs) does not force redoing it every time; only
# dualscreen's own build is meant to be cheap to repeat.
FFMPEG_VERSION="n7.1.5"
FFMPEG_DIR="${REPO_DIR}/.ffmpeg-steamos"
FFMPEG_SRC="${FFMPEG_DIR}/src"
FFMPEG_PREFIX="${FFMPEG_DIR}/prefix"

say()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m==>\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m==>\033[0m %s\n' "$*" >&2; exit 1; }

REBUILD=0
SHELL_ONLY=0
for argument in "$@"; do
    case "$argument" in
        --rebuild) REBUILD=1 ;;
        --shell)   SHELL_ONLY=1 ;;
        -h|--help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unrecognised argument: $argument" ;;
    esac
done

# Builds the minimal FFmpeg described above into $FFMPEG_PREFIX, unless a
# previous run already did. --disable-everything turns off every codec,
# format, filter and protocol; the --enable-* lines below turn back on
# exactly the four things dualscreen actually calls into (see
# src/video/video_encoder.cpp and video_decoder.cpp): the h264 decoder and
# parser, and the h264_vaapi encoder and hwaccel. avformat/avdevice/avfilter
# stay off entirely -- this project never links against them (see
# CMakeLists.txt's pkg_check_modules call, which only asks for
# libavcodec/libavutil/libswscale), so there is nothing here to build them
# for.
build_ffmpeg_if_needed() {
    if [ -f "${FFMPEG_PREFIX}/lib/pkgconfig/libavcodec.pc" ]; then
        return 0
    fi
    say "building a minimal FFmpeg (H.264 only) -- first run only, a few minutes"
    if [ ! -d "$FFMPEG_SRC" ]; then
        git clone --branch "$FFMPEG_VERSION" --depth 1 \
            https://github.com/FFmpeg/FFmpeg.git "$FFMPEG_SRC"
    fi
    (
        cd "$FFMPEG_SRC"
        ./configure \
            --prefix="$FFMPEG_PREFIX" \
            --disable-static --enable-shared \
            --disable-programs --disable-doc \
            --disable-avdevice --disable-avformat --disable-avfilter \
            --disable-swresample --disable-network \
            --disable-everything \
            --enable-avcodec --enable-avutil --enable-swscale \
            --enable-decoder=h264 \
            --enable-parser=h264 \
            --enable-encoder=h264_vaapi \
            --enable-hwaccel=h264_vaapi \
            --enable-vaapi
        make -j"$(nproc)"
        make install
    )
}

# Copies dualscreen's own three FFmpeg libraries -- not their dependencies,
# which are exactly the libva/libdrm/libX11 baseline described above and
# left to the host, the same as libGL -- to sit beside a binary that links
# them. $FFMPEG_PREFIX is not on the dynamic linker's normal search path (it
# was never registered with ldconfig, on purpose: this is a private prefix,
# not a system install), so LD_LIBRARY_PATH is what lets ldd resolve them
# here at all, inside the container or out.
#
# Called once per binary that links FFmpeg. cp refuses a file onto itself,
# and re-running this script after the first copy already landed would
# otherwise hit exactly that -- the -ef check is what skips a source that
# has already become the destination, rather than asking cp to prove it the
# hard way and abort the whole build under set -e.
bundle_ffmpeg_libs() {
    local binary="$1"
    local dest
    dest="$(dirname "$binary")"
    LD_LIBRARY_PATH="${FFMPEG_PREFIX}/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        ldd "$binary" 2>/dev/null | awk '/libav(codec|util)\.so|libswscale\.so/ {print $3}' |
        while read -r lib; do
            [ -n "$lib" ] && [ -f "$lib" ] || continue
            [ "$lib" -ef "$dest/$(basename "$lib")" ] && continue
            cp -uL "$lib" "$dest/"
        done
}

# The commands that actually do the build. Run inside the container, or
# directly if we are already somewhere with a compiler.
build_here() {
    if [ "$REBUILD" -eq 1 ]; then
        rm -rf "$BUILD_DIR"
    fi
    build_ffmpeg_if_needed
    PKG_CONFIG_PATH="${FFMPEG_PREFIX}/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
        cmake -S "$REPO_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build "$BUILD_DIR" -j"$(nproc)"
    bundle_ffmpeg_libs "$BUILD_DIR/dualscreen"
    bundle_ffmpeg_libs "$BUILD_DIR/dualscreen-companion"
}

install_dependencies() {
    sudo apt-get update -qq
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
        build-essential cmake pkg-config libsdl2-dev nlohmann-json3-dev \
        git nasm libva-dev libdrm-dev
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
    FFMPEG_VERSION='${FFMPEG_VERSION}' FFMPEG_DIR='${FFMPEG_DIR}' FFMPEG_SRC='${FFMPEG_SRC}' FFMPEG_PREFIX='${FFMPEG_PREFIX}'
    $(declare -f say)
    $(declare -f build_ffmpeg_if_needed)
    $(declare -f bundle_ffmpeg_libs)
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
