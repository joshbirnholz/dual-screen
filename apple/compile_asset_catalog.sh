#!/usr/bin/env bash
#
# The app icon, for a bundle built without Xcode.
#
# Xcode compiles an asset catalog by itself and merges what actool says about
# it into the bundle's Info.plist. A Ninja build has neither step, so
# CMakeLists.txt runs this after the link instead: compile the catalog into the
# bundle, then merge the keys actool writes (CFBundleIcons on iOS,
# TVTopShelfImage on an Apple TV) into Info.plist. Without those keys the
# pictures are in the bundle and the home screen still shows a blank icon.
#
# The keys are deleted before the merge: the bundle's Info.plist outlives a
# rebuild, and PlistBuddy will not merge over a key that is already there.
#
# Usage: compile_asset_catalog.sh <catalog> <bundle> <work dir> <actool args...>

set -euo pipefail

catalog="$1"
bundle="$2"
work="$3"
shift 3

partial="${work}/assetcatalog_generated_info.plist"
mkdir -p "${work}"

xcrun actool "${catalog}" \
    --compile "${bundle}" \
    --app-icon AppIcon \
    --output-partial-info-plist "${partial}" \
    --output-format human-readable-text \
    --errors --warnings \
    "$@"

plist="${bundle}/Info.plist"
for key in CFBundleIcons 'CFBundleIcons~ipad' TVTopShelfImage; do
    /usr/libexec/PlistBuddy -c "Delete :${key}" "${plist}" > /dev/null 2>&1 || true
done
/usr/libexec/PlistBuddy -c "Merge ${partial}" "${plist}"
