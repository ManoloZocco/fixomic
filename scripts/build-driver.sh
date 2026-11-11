#!/usr/bin/env bash
# Builds the fixomic HAL driver (BlackHole v0.7.2, GPL-3.0, configured through its preprocessor
# macros) as a Universal 2 bundle: build/driver/fixomic.driver
#
# The submodule driver/BlackHole is never modified: it is copied to build/driver/src, the
# patches in driver/patches/*.patch are applied to that copy (in lexical order), and the copy
# is built with xcodebuild. Upstream branding is replaced (device icon driver/resources/fixomic.icns,
# upstream README/CHANGELOG/VERSION/icon removed from Resources, version = fixomic's), then the
# bundle is signed ad-hoc. The GPL-3.0 LICENSE stays in the bundle.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UPSTREAM="$ROOT/driver/BlackHole"
PATCH_DIR="$ROOT/driver/patches"
OUT_DIR="$ROOT/build/driver"
SRC_DIR="$OUT_DIR/src"
XCODE_OUT="$OUT_DIR/xcode"
PRODUCT="$OUT_DIR/fixomic.driver"
ICON="$ROOT/driver/resources/fixomic.icns"
LOG="$OUT_DIR/xcodebuild.log"

BUNDLE_ID="it.manolozocco.fixomic.driver"
DEPLOYMENT_TARGET="14.4"
# Bundle version = fixomic's project version (project(fixomic VERSION x.y.z) in CMakeLists.txt).
VERSION="$(sed -nE 's/^project\(fixomic VERSION ([0-9.]+).*/\1/p' "$ROOT/CMakeLists.txt")"
[[ -n "$VERSION" ]] || { echo "error: cannot read the project version from CMakeLists.txt" >&2; exit 1; }
# BlackHole ships a fixed CFPlugIn factory UUID; every derived driver must use its own so that
# coreaudiod never sees two bundles registering the same factory.
FACTORY_UUID_UPSTREAM="e395c745-4eea-4d94-bb92-46224221047c"
FACTORY_UUID="3d76a877-64be-43f2-aa01-b4194a898640"

for tool in xcodebuild rsync patch lipo codesign strings /usr/libexec/PlistBuddy; do
    command -v "$tool" >/dev/null || { echo "error: '$tool' not found" >&2; exit 1; }
done
[[ -f "$UPSTREAM/BlackHole/BlackHole.c" ]] || {
    echo "error: $UPSTREAM is empty (git submodule update --init driver/BlackHole)" >&2; exit 1; }
[[ -f "$ICON" ]] || { echo "error: $ICON missing (scripts/make-driver-icon.sh)" >&2; exit 1; }

rm -rf "$SRC_DIR" "$XCODE_OUT" "$PRODUCT"
mkdir -p "$SRC_DIR"
rsync -a --exclude .git "$UPSTREAM/" "$SRC_DIR/"

shopt -s nullglob
# FIXOMIC_DRIVER_EXTRA_PATCHES: optional extra patch directory (local diagnostics only).
for p in "$PATCH_DIR"/*.patch ${FIXOMIC_DRIVER_EXTRA_PATCHES:+"$FIXOMIC_DRIVER_EXTRA_PATCHES"/*.patch}; do
    echo "applying $(basename "$p")"
    patch -d "$SRC_DIR" -p1 --forward --batch < "$p"
done
shopt -u nullglob

# String macros need literal quotes in the C source; xcodebuild splits a list setting on
# whitespace unless the element is quoted, hence '"\"fixomic Mic\""'.
DEFINES=(
    'kDriver_Name=\"fixomic\"'
    "kPlugIn_BundleID=\\\"$BUNDLE_ID\\\""
    'kHas_Driver_Name_Format=false'
    '"kDevice_Name=\"fixomic Mic\""'
    'kDevice_IsHidden=false'
    'kDevice_HasInput=true'
    'kDevice_HasOutput=false'
    '"kDevice2_Name=\"fixomic Bridge\""'
    'kDevice2_IsHidden=true'
    'kDevice2_HasInput=false'
    'kDevice2_HasOutput=true'
    'kNumber_Of_Channels=2'
    'kSampleRates=48000'
    'kCanBeDefaultDevice=false'
    'kCanBeDefaultSystemDevice=false'
    'kManufacturer_Name=\"fixomic\"'
    'kPlugIn_Icon=\"fixomic.icns\"'
)

echo "xcodebuild (log: $LOG)"
set +e
xcodebuild \
    -project "$SRC_DIR/BlackHole.xcodeproj" \
    -target BlackHole \
    -configuration Release \
    CONFIGURATION_BUILD_DIR="$XCODE_OUT" \
    SYMROOT="$OUT_DIR/xcode-sym" \
    OBJROOT="$OUT_DIR/xcode-obj" \
    PRODUCT_NAME=fixomic \
    PRODUCT_BUNDLE_IDENTIFIER="$BUNDLE_ID" \
    MACOSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
    ARCHS="arm64 x86_64" \
    ONLY_ACTIVE_ARCH=NO \
    CODE_SIGNING_ALLOWED=NO \
    CODE_SIGN_IDENTITY="" \
    DEVELOPMENT_TEAM="" \
    GCC_PREPROCESSOR_DEFINITIONS="\$(inherited) ${DEFINES[*]}" \
    > "$LOG" 2>&1
status=$?
set -e
grep -E ' error: |\*\*' "$LOG" || true
if [[ $status -ne 0 ]]; then
    echo "error: xcodebuild failed with status $status (see $LOG)" >&2
    exit "$status"
fi

[[ -f "$XCODE_OUT/fixomic.driver/Contents/MacOS/fixomic" ]] || {
    echo "error: xcodebuild did not produce fixomic.driver/Contents/MacOS/fixomic" >&2; exit 1; }
mv "$XCODE_OUT/fixomic.driver" "$PRODUCT"

# Replace upstream resources: keep only the GPL LICENSE, add our icon.
RES="$PRODUCT/Contents/Resources"
rm -f "$RES/BlackHole.icns" "$RES/README.md" "$RES/CHANGELOG.md" "$RES/VERSION"
cp "$ICON" "$RES/fixomic.icns"

PLIST="$PRODUCT/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $VERSION" \
    -c "Set :CFBundleVersion $VERSION" \
    "$PLIST"
/usr/libexec/PlistBuddy -c "Delete :CFPlugInFactories:$FACTORY_UUID_UPSTREAM" \
    -c "Add :CFPlugInFactories:$FACTORY_UUID string BlackHole_Create" \
    -c "Delete :CFPlugInTypes:443ABAB8-E7B3-491A-B985-BEB9187030DB:0" \
    -c "Add :CFPlugInTypes:443ABAB8-E7B3-491A-B985-BEB9187030DB:0 string $FACTORY_UUID" \
    "$PLIST"

codesign --force --sign - --deep "$PRODUCT"

# ---- verification ----------------------------------------------------------------------------
fail() { echo "error: $*" >&2; exit 1; }
BIN="$PRODUCT/Contents/MacOS/fixomic"
archs="$(lipo -archs "$BIN")"
[[ " $archs " == *" arm64 "* && " $archs " == *" x86_64 "* ]] || fail "not universal: $archs"
plist_id="$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$PLIST")"
[[ "$plist_id" == "$BUNDLE_ID" ]] || fail "CFBundleIdentifier is '$plist_id'"
BIN_STRINGS="$(strings -a "$BIN")"
for s in "fixomic Mic" "fixomic Bridge" "fixomic_UID" "fixomic_2_UID" "$BUNDLE_ID" "fixomic.icns"; do
    grep -qxF "$s" <<<"$BIN_STRINGS" || fail "string '$s' missing from the binary (macro quoting?)"
done
for s in "Existential" "BlackHole Box" "BlackHole.icns"; do
    ! grep -qF "$s" <<<"$BIN_STRINGS" || fail "upstream branding '$s' still in the binary"
done
resources="$(cd "$RES" && LC_ALL=C ls | tr '\n' ' ')"
[[ "$resources" == "LICENSE fixomic.icns " ]] || fail "unexpected resources: $resources"
codesign --verify --deep --strict "$PRODUCT"

echo "built $PRODUCT ($archs, $plist_id $VERSION, factory $FACTORY_UUID)"
