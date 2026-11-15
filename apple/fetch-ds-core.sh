#!/usr/bin/env bash
# Makes the DS core an iPhone or iPad build carries: melonDS DS, the libretro
# core the desktop app plays DS games with, wrapped as
# melondsds_libretro.framework -- the form iOS lets an app carry code of its
# own in. apple/CMakeLists.txt finds it at
#
#   build-apple/ds-core/<sdk>/melondsds_libretro.framework
#
# and puts it in the app's Frameworks folder; the app loads it from there when
# a game starts (DS_BUNDLED_LIBRETRO_CORE).
#
# It is built from melonDS DS's own source, at the tag below, with its JIT
# off: iOS gives an app no memory it may write and then run, so melonDS runs
# on its interpreter there (melonDS DS turns the JIT off for every iOS build
# anyway). The first build takes a few minutes; after that it is kept.
#
# usage: apple/fetch-ds-core.sh [--simulator] [--nightly] [--dylib <file>]
#                               [--out <dir>] [--force]
#
#   --simulator     for the iOS simulator rather than a device
#   --nightly       download libretro's nightly iOS build instead, where
#                   there is one (device only)
#   --dylib <file>  wrap this already-built melondsds_libretro_ios.dylib
#   --out <dir>     where build-apple/ is (default: the repository's)
#   --force         make it again even if it is already there

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "${here}/.." && pwd)"
out="${root}/build-apple"
sdk="iphoneos"
from_source=1
dylib=""
force=0
melonds_tag="${MELONDS_DS_TAG:-v1.4.0}"
nightly="https://buildbot.libretro.com/nightly/apple/ios-arm64/latest/melondsds_libretro_ios.dylib.zip"
min_os="17.0"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --simulator) sdk="iphonesimulator"; shift ;;
        --from-source) from_source=1; shift ;;
        --nightly) from_source=0; shift ;;
        --dylib) dylib="${2:-}"; shift 2 ;;
        --out) out="${2:-}"; shift 2 ;;
        --force) force=1; shift ;;
        -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "This needs a Mac: the core is checked and signed with Apple's tools." >&2
    exit 1
fi

dest="${out}/ds-core/${sdk}"
framework="${dest}/melondsds_libretro.framework"
if [[ -e "${framework}/melondsds_libretro" && ${force} -eq 0 ]]; then
    echo "DS core: ${framework} is already there (--force to make it again)"
    exit 0
fi

work="${out}/ds-core/work-${sdk}"
rm -rf "${work}"
mkdir -p "${work}" "${dest}"

if [[ -z "${dylib}" ]]; then
    if [[ "${sdk}" == "iphonesimulator" ]]; then
        from_source=1
    fi
    if [[ ${from_source} -eq 0 ]]; then
        echo "=== downloading melonDS DS for iOS from libretro's nightly builds"
        curl -fL --retry 3 -o "${work}/core.zip" "${nightly}"
        (cd "${work}" && unzip -q core.zip)
        dylib="$(find "${work}" -name '*.dylib' -print -quit)"
    else
        echo "=== building melonDS DS ${melonds_tag} for ${sdk} (a few minutes, once)"
        git clone --depth 1 -b "${melonds_tag}" https://github.com/JesseTG/melonds-ds "${work}/melonds-ds"
        platform="OS64"
        if [[ "${sdk}" == "iphonesimulator" ]]; then platform="SIMULATORARM64"; fi
        cmake -S "${work}/melonds-ds" -B "${work}/build" -G Ninja \
              --toolchain "${work}/melonds-ds/cmake/toolchain/ios.toolchain.cmake" \
              -DPLATFORM="${platform}" -DDEPLOYMENT_TARGET=13 \
              -DBUILD_AS_SHARED_LIBRARY=ON -DENABLE_JIT=OFF \
              -DCMAKE_BUILD_TYPE=Release -Wno-deprecated
        cmake --build "${work}/build"
        dylib="$(find "${work}/build" -name 'melondsds_libretro*.dylib' -print -quit)"
    fi
fi

if [[ -z "${dylib}" || ! -f "${dylib}" ]]; then
    echo "no melonDS DS library was found to wrap" >&2
    exit 1
fi
echo "=== wrapping ${dylib}"
archs="$(lipo -archs "${dylib}")"
if [[ " ${archs} " != *" arm64 "* ]]; then
    echo "${dylib} is built for ${archs}, not arm64" >&2
    exit 1
fi

rm -rf "${framework}"
mkdir -p "${framework}"
cp "${dylib}" "${framework}/melondsds_libretro"
chmod 755 "${framework}/melondsds_libretro"
# Its own name, as the app's loader will look it up.
install_name_tool -id "@rpath/melondsds_libretro.framework/melondsds_libretro" \
                  "${framework}/melondsds_libretro"

platform_name="iPhoneOS"
if [[ "${sdk}" == "iphonesimulator" ]]; then platform_name="iPhoneSimulator"; fi
cat > "${framework}/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDevelopmentRegion</key>
	<string>en</string>
	<key>CFBundleExecutable</key>
	<string>melondsds_libretro</string>
	<key>CFBundleIdentifier</key>
	<string>com.joshbirnholz.dualscreen.melondsds</string>
	<key>CFBundleInfoDictionaryVersion</key>
	<string>6.0</string>
	<key>CFBundleName</key>
	<string>melondsds_libretro</string>
	<key>CFBundlePackageType</key>
	<string>FMWK</string>
	<key>CFBundleShortVersionString</key>
	<string>1.0</string>
	<key>CFBundleVersion</key>
	<string>1</string>
	<key>CFBundleSupportedPlatforms</key>
	<array>
		<string>${platform_name}</string>
	</array>
	<key>MinimumOSVersion</key>
	<string>${min_os}</string>
</dict>
</plist>
PLIST
plutil -lint "${framework}/Info.plist" >/dev/null

# What it was made from, for the log of anyone asking which core this is.
if [[ -n "${dylib}" ]]; then
    shasum -a 256 "${framework}/melondsds_libretro" | awk '{print $1}' > "${dest}/sha256.txt"
fi
otool -L "${framework}/melondsds_libretro" | sed -n '2,20p'
rm -rf "${work}"
echo "DS core: ${framework}"
