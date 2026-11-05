#!/usr/bin/env bash
#
# Builds Dualscreen for 64-bit Windows, cross-compiled from Linux with
# MinGW-w64 -- the same way in CI as on a developer's machine.
#
#     ./tools/build-windows.sh            # build into build-windows/
#     ./tools/build-windows.sh --package  # ...and make dualscreen-windows.zip
#
# Needs (Debian/Ubuntu): g++-mingw-w64-x86-64-posix mingw-w64-tools cmake
# ninja-build nasm pkg-config curl git zip. Wine, if installed, runs the tests.
#
# Everything Dualscreen links is built or fetched once into .deps-windows/
# (cached in CI, keyed on this file): SDL2's official MinGW build, zlib and
# zstd statically, and a minimal FFmpeg the way tools/build-steamos.sh makes
# one -- H.264 decode, and encoding only through the GPU vendors' and
# Windows's own hardware encoders (Media Foundation, NVENC, AMF), so it stays
# LGPL and needs no libx264. NVENC and AMF are header-only here; the drivers
# bring the rest at run time, and a PC without them just skips that encoder.

set -euo pipefail

REPO_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_DIR}/build-windows}"
DEPS="${DUALSCREEN_WINDOWS_DEPS:-${REPO_DIR}/.deps-windows}"
SRC="${DEPS}/src"
PREFIX="${DEPS}/prefix"
HOST=x86_64-w64-mingw32

SDL2_VERSION=2.32.10
ZLIB_VERSION=1.3.1
ZSTD_VERSION=1.5.6
FFMPEG_VERSION=n7.1.5
NV_HEADERS_VERSION=n12.2.72.0
AMF_VERSION=v1.4.36

PACKAGE=0
for argument in "$@"; do
    case "$argument" in
        --package) PACKAGE=1 ;;
        -h|--help) sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unrecognised argument: $argument" >&2; exit 1 ;;
    esac
done

say() { printf '\033[1;36m==>\033[0m %s\n' "$*"; }

CC="$(command -v ${HOST}-gcc-posix || command -v ${HOST}-gcc)"
CXX="$(command -v ${HOST}-g++-posix || command -v ${HOST}-g++)"
JOBS="$(nproc)"
mkdir -p "$SRC" "$PREFIX"

fetch() {  # url file
    [ -f "$SRC/$2" ] || curl -fsSL --retry 4 -o "$SRC/$2" "$1"
}

cmake_dep() {  # source-dir build-dir [extra cmake args...]
    local source="$1" build="$2"; shift 2
    cmake -S "$source" -B "$build" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$REPO_DIR/cmake/mingw-w64-x86_64.cmake" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" "$@"
    cmake --build "$build" -j"$JOBS"
    cmake --install "$build"
}

if [ ! -f "$PREFIX/lib/libSDL2.dll.a" ]; then
    say "SDL2 $SDL2_VERSION"
    fetch "https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-devel-${SDL2_VERSION}-mingw.tar.gz" sdl2.tar.gz
    tar -C "$SRC" -xzf "$SRC/sdl2.tar.gz"
    cp -r "$SRC/SDL2-${SDL2_VERSION}/${HOST}/." "$PREFIX/"
    # The .pc and CMake files name the prefix SDL2 was built in; point them here.
    sed -i "s|^prefix=.*|prefix=${PREFIX}|" "$PREFIX/lib/pkgconfig/sdl2.pc"
fi

if [ ! -f "$PREFIX/lib/libzlibstatic.a" ] && [ ! -f "$PREFIX/lib/libz.a" ]; then
    say "zlib $ZLIB_VERSION"
    fetch "https://github.com/madler/zlib/releases/download/v${ZLIB_VERSION}/zlib-${ZLIB_VERSION}.tar.gz" zlib.tar.gz
    tar -C "$SRC" -xzf "$SRC/zlib.tar.gz"
    cmake_dep "$SRC/zlib-${ZLIB_VERSION}" "$SRC/zlib-build"
    # Static only: one less DLL to ship, and FindZLIB then cannot pick the DLL.
    rm -f "$PREFIX/lib/libzlib.dll.a" "$PREFIX/bin/libzlib.dll"
    cp "$PREFIX/lib/libzlibstatic.a" "$PREFIX/lib/libz.a"
fi

if [ ! -f "$PREFIX/lib/libzstd.a" ]; then
    say "zstd $ZSTD_VERSION"
    fetch "https://github.com/facebook/zstd/releases/download/v${ZSTD_VERSION}/zstd-${ZSTD_VERSION}.tar.gz" zstd.tar.gz
    tar -C "$SRC" -xzf "$SRC/zstd.tar.gz"
    cmake_dep "$SRC/zstd-${ZSTD_VERSION}/build/cmake" "$SRC/zstd-build" \
        -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF
fi

if [ ! -f "$PREFIX/include/ffnvcodec/nvEncodeAPI.h" ]; then
    say "NVENC headers $NV_HEADERS_VERSION"
    rm -rf "$SRC/nv-codec-headers"
    git clone -q --depth 1 --branch "$NV_HEADERS_VERSION" \
        https://github.com/FFmpeg/nv-codec-headers.git "$SRC/nv-codec-headers"
    make -C "$SRC/nv-codec-headers" PREFIX="$PREFIX" install > /dev/null
fi

if [ ! -f "$PREFIX/include/AMF/core/Factory.h" ]; then
    say "AMF headers $AMF_VERSION"
    rm -rf "$SRC/amf"
    git clone -q --depth 1 --branch "$AMF_VERSION" --filter=blob:none --sparse \
        https://github.com/GPUOpen-LibrariesAndSDKs/AMF.git "$SRC/amf"
    git -C "$SRC/amf" sparse-checkout set amf/public/include
    mkdir -p "$PREFIX/include/AMF"
    cp -r "$SRC/amf/amf/public/include/." "$PREFIX/include/AMF/"
fi

if [ ! -f "$PREFIX/lib/pkgconfig/libavcodec.pc" ]; then
    say "FFmpeg $FFMPEG_VERSION (H.264 only) -- first run only, a few minutes"
    [ -d "$SRC/ffmpeg" ] || git clone -q --branch "$FFMPEG_VERSION" --depth 1 \
        https://github.com/FFmpeg/FFmpeg.git "$SRC/ffmpeg"
    (
        cd "$SRC/ffmpeg"
        PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" ./configure \
            --prefix="$PREFIX" \
            --enable-cross-compile --target-os=mingw32 --arch=x86_64 \
            --cross-prefix=${HOST}- --cc="$CC" --cxx="$CXX" \
            --pkg-config=pkg-config \
            --extra-cflags="-I$PREFIX/include" \
            --disable-static --enable-shared \
            --disable-programs --disable-doc --disable-debug \
            --disable-avdevice --disable-avformat --disable-avfilter \
            --disable-swresample --disable-network \
            --disable-everything \
            --enable-avcodec --enable-avutil --enable-swscale \
            --enable-decoder=h264 --enable-parser=h264 \
            --enable-hwaccel=h264_d3d11va --enable-hwaccel=h264_d3d11va2 \
            --enable-hwaccel=h264_dxva2 \
            --enable-d3d11va --enable-dxva2 \
            --enable-mediafoundation --enable-encoder=h264_mf \
            --enable-ffnvcodec --enable-nvenc --enable-encoder=h264_nvenc \
            --enable-amf --enable-encoder=h264_amf
        make -j"$JOBS"
        make install
    )
fi

say "Dualscreen"
export DUALSCREEN_WINDOWS_DEPS="$DEPS/prefix"
PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" PKG_CONFIG_SYSROOT_DIR="" \
cmake -S "$REPO_DIR" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$REPO_DIR/cmake/mingw-w64-x86_64.cmake" \
    -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-RelWithDebInfo}" \
    -DCMAKE_PREFIX_PATH="$PREFIX" \
    -DDUALSCREEN_WINDOWS_DEPS="$PREFIX" \
    -DSDL2_DIR="$PREFIX/lib/cmake/SDL2" \
    ${DUALSCREEN_CMAKE_ARGS:-}
cmake --build "$BUILD_DIR" -j"$JOBS"

# The DLLs a Windows binary needs beside it: SDL2's, FFmpeg's, and MinGW's own
# thread library (the C++ runtime is linked in statically).
copy_runtime() {  # destination
    local dest="$1"
    cp "$PREFIX/bin/SDL2.dll" "$dest/"
    cp "$PREFIX"/bin/avcodec-*.dll "$PREFIX"/bin/avutil-*.dll "$PREFIX"/bin/swscale-*.dll "$dest/"
    local pthread
    pthread="$("$CC" -print-file-name=libwinpthread-1.dll)"
    [ -f "$pthread" ] || pthread="$(find /usr/${HOST} /usr/lib/gcc/${HOST} -name libwinpthread-1.dll 2>/dev/null | head -n 1)"
    [ -n "$pthread" ] && cp "$pthread" "$dest/"
}
copy_runtime "$BUILD_DIR"

if [ "$PACKAGE" -eq 1 ]; then
    say "dualscreen-windows.zip"
    stage="$BUILD_DIR/package/Dualscreen"
    rm -rf "$BUILD_DIR/package"
    mkdir -p "$stage"
    cp "$BUILD_DIR/dualscreen.exe" "$BUILD_DIR/dualscreen-companion.exe" "$stage/"
    copy_runtime "$stage"
    ${HOST}-strip --strip-debug "$stage"/*.exe
    (cd "$BUILD_DIR/package" && rm -f "$REPO_DIR/dualscreen-windows.zip" && zip -qr "$REPO_DIR/dualscreen-windows.zip" Dualscreen)
    ls -la "$REPO_DIR/dualscreen-windows.zip"
fi
