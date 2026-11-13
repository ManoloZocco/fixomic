#!/usr/bin/env bash
# Builds a fixomic release: fixomic.app as a Universal 2 (arm64 + x86_64) Release build, with the
# HAL driver inside, signed ad-hoc and zipped:
#   dist/fixomic-<version>.zip          (version = project(fixomic VERSION x.y.z) in CMakeLists.txt)
#   dist/fixomic-<version>.zip.sha256   (shasum -a 256 -c fixomic-<version>.zip.sha256)
#
#   scripts/package-release.sh           incremental: build-release/ is reused
#   scripts/package-release.sh --clean   wipes build-release/ first
#
# Everything is built under build-release/ (the CMake build and the driver); the dev build in build/
# is never written to. The universal WebRTC APM comes from scripts/build-webrtc-apm.sh, run here only
# when it is missing (that script works in build/deps and third_party/install). The JUCE and Catch2
# checkouts of the dev build are reused when they are clean and at the pinned tag, otherwise CMake
# clones them.
#
# Signing is ad-hoc, inside-out (the driver, then the app), whatever identity the dev builds use,
# and without the hardened runtime: an ad-hoc release is never notarized, so the hardened runtime
# would buy nothing with Gatekeeper, and the AU / VST3 plugins the app loads in-process would then
# need entitlements (library validation off, some also JIT or unsigned executable memory).
#
# The licence texts of everything compiled in (see NOTICE) go into Contents/Resources/Licenses.
# The script then unpacks the zip and checks what it ships, failing on the first problem: both
# architectures in every binary, the minimum macOS, the driver where the app's installer looks for
# it, the licences, the ad-hoc signature, no developer commands, no quarantine attribute.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REL="$ROOT/build-release"
DIST="$ROOT/dist"
STAGE_DIR="$REL/package"
CHECK_DIR="$REL/package-check"
DRIVER_OUT="$REL/driver"
ENTITLEMENTS="$ROOT/cmake/fixomic.entitlements"
APM_LIB="$ROOT/third_party/install/webrtc-apm/universal/lib/libwebrtc-audio-processing-2.a"
ARCHS="arm64 x86_64"

# The driver's local diagnostic patches (scripts/build-driver.sh) never go into a release.
unset FIXOMIC_DRIVER_EXTRA_PATCHES

fail() { echo "error: $*" >&2; exit 1; }
step() { printf '\n==> %s\n' "$*"; }
ok() { echo "ok: $*"; }

usage() { echo "usage: scripts/package-release.sh [--clean]" >&2; exit 2; }
CLEAN=0
for arg in "$@"; do
    case "$arg" in
        --clean) CLEAN=1 ;;
        *) usage ;;
    esac
done

for tool in cmake ninja git lipo codesign ditto xattr shasum strings vtool zipinfo unzip /usr/libexec/PlistBuddy; do
    command -v "$tool" >/dev/null || fail "'$tool' not found"
done

VERSION="$(sed -nE 's/^project\(fixomic VERSION ([0-9.]+).*/\1/p' "$ROOT/CMakeLists.txt")"
[[ -n "$VERSION" ]] || fail "cannot read the project version from CMakeLists.txt"
DEPLOYMENT_TARGET="$(sed -nE 's/^set\(CMAKE_OSX_DEPLOYMENT_TARGET "([0-9.]+)".*/\1/p' "$ROOT/CMakeLists.txt")"
[[ -n "$DEPLOYMENT_TARGET" ]] || fail "cannot read CMAKE_OSX_DEPLOYMENT_TARGET from CMakeLists.txt"
# What DriverInstaller checks before installing (kDriverBundleId).
DRIVER_BUNDLE_ID="$(sed -nE 's/.*kDriverBundleId = "([^"]+)".*/\1/p' "$ROOT/src/platform/mac/DriverInstaller.h")"
[[ -n "$DRIVER_BUNDLE_ID" ]] || fail "cannot read kDriverBundleId from src/platform/mac/DriverInstaller.h"
ZIP_NAME="fixomic-$VERSION.zip"
ZIP="$DIST/$ZIP_NAME"

TIMINGS=()
timed() {
    local label="$1" start=$SECONDS
    shift
    "$@"
    TIMINGS+=("$(printf '%-24s %5d s' "$label" $((SECONDS - start)))")
}

# Exactly the architectures in $ARCHS, in any order. (lipo -verify_arch takes only one architecture
# on current Xcode, hence the comparison.)
sorted_archs() { tr ' ' '\n' | grep . | sort | tr '\n' ' '; }
WANT_ARCHS="$(sorted_archs <<<"$ARCHS")"
is_universal() { [[ -f "$1" && "$(lipo -archs "$1" 2>/dev/null | sorted_archs)" == "$WANT_ARCHS" ]]; }
check_universal() {
    local what="$1" file="$2"
    echo "$what: $(lipo -info "$file")"
    is_universal "$file" || fail "$what is not universal: $(lipo -archs "$file")"
}

# Every slice of a Mach-O built for the deployment target.
check_minos() {
    local what="$1" file="$2" arch minos
    for arch in $ARCHS; do
        minos="$(vtool -arch "$arch" -show-build "$file" | awk '$1 == "minos" && !seen { print $2; seen = 1 }')"
        [[ "$minos" == "$DEPLOYMENT_TARGET" ]] || fail "$what ($arch) has minos '$minos', expected $DEPLOYMENT_TARGET"
    done
    ok "$what: minos $DEPLOYMENT_TARGET on $ARCHS"
}

plist_value() { /usr/libexec/PlistBuddy -c "Print :$2" "$1" 2>/dev/null || true; }

# A checkout of the dev build that CMake may use instead of cloning: clean and at the tag pinned in
# cmake/Dependencies.cmake. Prints the -D / -U argument for FetchContent.
fetchcontent_source_arg() {
    local name="$1" src="$2" var tag
    var="FETCHCONTENT_SOURCE_DIR_$(tr '[:lower:]' '[:upper:]' <<<"$name")"
    tag="$(awk -v name="$name" '/FetchContent_Declare\(/ { getline; dep = $1 } $1 == "GIT_TAG" && dep == name { print $2 }' \
        "$ROOT/cmake/Dependencies.cmake")"
    if [[ -n "$tag" && -f "$src/CMakeLists.txt" ]] \
        && [[ "$(git -C "$src" describe --tags --exact-match 2>/dev/null)" == "$tag" ]] \
        && [[ -z "$(git -C "$src" status --porcelain 2>/dev/null)" ]]; then
        echo "-D$var=$src"
    else
        echo "-U$var"
    fi
}

# ---- dependencies --------------------------------------------------------------------------------
deps() {
    step "WebRTC APM (universal)"
    if ! is_universal "$APM_LIB"; then
        echo "$APM_LIB missing or not universal: running scripts/build-webrtc-apm.sh"
        "$ROOT/scripts/build-webrtc-apm.sh"
    fi
    check_universal "APM" "$APM_LIB"

    step "driver (scripts/build-driver.sh -> build-release/driver)"
    FIXOMIC_DRIVER_OUT_DIR="$DRIVER_OUT" "$ROOT/scripts/build-driver.sh"
}

# ---- Release build ------------------------------------------------------------------------------
configure() {
    step "configure build-release (Release, $ARCHS, ad-hoc)"
    local juce_arg catch2_arg
    juce_arg="$(fetchcontent_source_arg JUCE "$ROOT/build/_deps/juce-src")"
    catch2_arg="$(fetchcontent_source_arg Catch2 "$ROOT/build/_deps/catch2-src")"
    echo "JUCE: $juce_arg"
    echo "Catch2: $catch2_arg"
    cmake -S "$ROOT" -B "$REL" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        "-DCMAKE_OSX_ARCHITECTURES=${ARCHS// /;}" \
        "-DCMAKE_OSX_DEPLOYMENT_TARGET=$DEPLOYMENT_TARGET" \
        -DFIXOMIC_CODESIGN_IDENTITY=- \
        -DFIXOMIC_ASAN=OFF \
        "-DFIXOMIC_DRIVER_BUNDLE=$DRIVER_OUT/fixomic.driver" \
        "$juce_arg" "$catch2_arg"

    # Every compile command targets both architectures and the deployment target.
    local db="$REL/compile_commands.json" total missing
    [[ -f "$db" ]] || fail "$db missing"
    total="$(grep -c '"command":' "$db")"
    missing="$(grep '"command":' "$db" | grep -v -e "-mmacosx-version-min=$DEPLOYMENT_TARGET" | grep -c . || true)"
    [[ "$missing" -eq 0 ]] || fail "$missing of $total compile commands lack -mmacosx-version-min=$DEPLOYMENT_TARGET"
    for arch in $ARCHS; do
        missing="$(grep '"command":' "$db" | grep -v -e "-arch $arch" | grep -c . || true)"
        [[ "$missing" -eq 0 ]] || fail "$missing of $total compile commands lack -arch $arch"
    done
    ok "all $total compile commands: -mmacosx-version-min=$DEPLOYMENT_TARGET, -arch ${ARCHS// /, -arch }"
}

build() {
    step "build fixomic (Release)"
    cmake --build "$REL" --target fixomic
}

# ---- licences ------------------------------------------------------------------------------------
# The licence texts travel with the binary (NOTICE lists them): fixomic's, BlackHole's, and those of
# the third-party code compiled into the app, taken from the very sources it was built from: the
# JUCE checkout CMake used and the APM source scripts/build-webrtc-apm.sh cloned.
LICENSE_COUNT=0
add_licenses() {
    local dest="$1" juce apm abseil
    juce="$(sed -nE 's/^JUCE_SOURCE_DIR:STATIC=(.*)/\1/p' "$REL/CMakeCache.txt")"
    [[ -d "$juce/modules" ]] || fail "cannot find the JUCE source (JUCE_SOURCE_DIR in $REL/CMakeCache.txt)"
    apm="$ROOT/build/deps/webrtc-apm-src"
    abseil="$(sed -nE 's/^directory *= *([^ ]+).*/\1/p' "$apm/subprojects/abseil-cpp.wrap" 2>/dev/null || true)"
    [[ -n "$abseil" ]] || fail "cannot find the APM source with its abseil wrap in $apm (scripts/build-webrtc-apm.sh)"
    local files=(
        "$ROOT/LICENSE"                                                          fixomic-LICENSE.txt
        "$ROOT/NOTICE"                                                           NOTICE.txt
        "$ROOT/driver/LICENSE"                                                   BlackHole-LICENSE.txt
        "$juce/LICENSE.md"                                                       JUCE-LICENSE.md
        "$juce/modules/juce_audio_processors_headless/format_types/VST3_SDK/LICENSE.txt" VST3-SDK-LICENSE.txt
        "$juce/modules/juce_graphics/fonts/harfbuzz/COPYING"                     HarfBuzz-COPYING.txt
        "$juce/modules/juce_graphics/unicode/sheenbidi/LICENSE"                  SheenBidi-LICENSE.txt
        "$juce/modules/juce_graphics/image_formats/pnglib/LICENSE"               libpng-LICENSE.txt
        "$juce/modules/juce_graphics/image_formats/jpglib/README"                libjpeg-README.txt
        "$juce/modules/juce_core/zip/zlib/README"                                zlib-README.txt
        "$apm/webrtc/LICENSE"                                                    WebRTC-LICENSE.txt
        "$apm/webrtc/PATENTS"                                                    WebRTC-PATENTS.txt
        "$apm/subprojects/$abseil/LICENSE"                                       Abseil-LICENSE.txt
        "$apm/webrtc/third_party/rnnoise/COPYING"                                RNNoise-COPYING.txt
        "$apm/webrtc/third_party/pffft/LICENSE"                                  PFFFT-LICENSE.txt
        "$apm/webrtc/common_audio/third_party/ooura/LICENSE"                     Ooura-FFT-LICENSE.txt
        "$apm/webrtc/common_audio/third_party/spl_sqrt_floor/LICENSE"            spl_sqrt_floor-LICENSE.txt
    )
    mkdir -p "$dest"
    local i
    for ((i = 0; i < ${#files[@]}; i += 2)); do
        [[ -f "${files[i]}" ]] || fail "licence file missing: ${files[i]}"
        cp "${files[i]}" "$dest/${files[i + 1]}"
    done
    LICENSE_COUNT=$((${#files[@]} / 2))
    echo "$LICENSE_COUNT licence files in ${dest#"$STAGE_DIR"/}"
}

# ---- sign and zip --------------------------------------------------------------------------------
package() {
    local built="$REL/fixomic_artefacts/Release/fixomic.app" app="$STAGE_DIR/fixomic.app"
    [[ -x "$built/Contents/MacOS/fixomic" ]] || fail "$built was not built"
    [[ -d "$built/Contents/Resources/fixomic.driver" ]] || fail "$built does not carry the driver"

    step "stage, add the licences, sign ad-hoc (driver, then app)"
    rm -rf "$STAGE_DIR"
    mkdir -p "$STAGE_DIR"
    ditto --noqtn "$built" "$app"
    add_licenses "$app/Contents/Resources/Licenses"
    codesign --force --sign - --timestamp=none "$app/Contents/Resources/fixomic.driver"
    codesign --force --sign - --timestamp=none --entitlements "$ENTITLEMENTS" "$app"

    step "zip"
    mkdir -p "$DIST"
    rm -f "$ZIP" "$ZIP.sha256"
    # No resource forks, extended attributes or ACLs: no __MACOSX entries, so nothing can carry
    # com.apple.quarantine (or com.apple.provenance) into the archive.
    ditto -c -k --norsrc --noextattr --noacl --keepParent "$app" "$ZIP"
    (cd "$DIST" && shasum -a 256 "$ZIP_NAME" > "$ZIP_NAME.sha256")
}

# ---- checks, on the unpacked zip -----------------------------------------------------------------
checks() {
    step "checks (on the unpacked zip)"
    unzip -tq "$ZIP" >/dev/null || fail "$ZIP is corrupt"
    (cd "$DIST" && shasum -a 256 -c "$ZIP_NAME.sha256")

    local entries
    entries="$(zipinfo -1 "$ZIP")"
    ! grep -qE '(^|/)(__MACOSX/|\._)' <<<"$entries" || fail "the zip has AppleDouble entries (extended attributes)"
    [[ "$(head -1 <<<"$entries")" == "fixomic.app/" ]] || fail "the zip does not start with fixomic.app/"
    ! xattr -p com.apple.quarantine "$ZIP" >/dev/null 2>&1 || fail "$ZIP itself is quarantined"

    rm -rf "$CHECK_DIR"
    mkdir -p "$CHECK_DIR"
    ditto -x -k "$ZIP" "$CHECK_DIR"
    local app="$CHECK_DIR/fixomic.app"
    local bin="$app/Contents/MacOS/fixomic"
    local driver="$app/Contents/Resources/fixomic.driver"
    [[ -x "$bin" ]] || fail "the zip has no fixomic.app/Contents/MacOS/fixomic"
    local attrs
    attrs="$(xattr -r "$app" 2>/dev/null || true)"
    ! grep -q 'com.apple.quarantine' <<<"$attrs" || fail "the unpacked app carries com.apple.quarantine"
    ok "no com.apple.quarantine: no AppleDouble entries in the zip ($(grep -c . <<<"$entries") entries), none on the zip, none unpacked"

    check_universal "app binary" "$bin"
    check_universal "driver binary" "$driver/Contents/MacOS/fixomic"
    echo "scan worker: the app binary itself (PluginCatalog relaunches it with --scan-worker:<pipe>), checked above"
    check_minos "app binary" "$bin"
    check_minos "driver binary" "$driver/Contents/MacOS/fixomic"

    local plist="$app/Contents/Info.plist" value
    value="$(plist_value "$plist" LSMinimumSystemVersion)"
    [[ "$value" == "$DEPLOYMENT_TARGET" ]] || fail "LSMinimumSystemVersion is '$value', expected $DEPLOYMENT_TARGET"
    value="$(plist_value "$plist" CFBundleShortVersionString)"
    [[ "$value" == "$VERSION" ]] || fail "app CFBundleShortVersionString is '$value', expected $VERSION"
    ok "Info.plist: LSMinimumSystemVersion $DEPLOYMENT_TARGET, version $VERSION, LSUIElement $(plist_value "$plist" LSUIElement)"

    # DriverInstaller: bundledDriverLocation() = <resourcePath>/fixomic.driver, bundledDriverVersion() =
    # its CFBundleShortVersionString, and an install needs CFBundleIdentifier == kDriverBundleId.
    value="$(plist_value "$driver/Contents/Info.plist" CFBundleIdentifier)"
    [[ "$value" == "$DRIVER_BUNDLE_ID" ]] || fail "bundled driver CFBundleIdentifier is '$value', expected $DRIVER_BUNDLE_ID"
    value="$(plist_value "$driver/Contents/Info.plist" CFBundleShortVersionString)"
    [[ "$value" == "$VERSION" ]] || fail "bundled driver version is '$value', expected $VERSION"
    ok "driver at Contents/Resources/fixomic.driver: $DRIVER_BUNDLE_ID $value (bundledDriverVersion() = '$value')"

    value="$(find "$app/Contents/Resources/Licenses" -type f | grep -c . || true)"
    [[ "$value" -eq "$LICENSE_COUNT" ]] || fail "$value licence files in the app, expected $LICENSE_COUNT"
    [[ -f "$driver/Contents/Resources/LICENSE" ]] || fail "the driver lost its GPL-3.0 LICENSE"
    ok "licences: $value files in Contents/Resources/Licenses, GPL-3.0 LICENSE in the driver"

    codesign --verify --deep --strict --verbose=2 "$app"
    codesign --verify --strict --verbose=2 "$driver"
    local info
    for target in "$app" "$driver"; do
        info="$(codesign -dv "$target" 2>&1)"
        grep -q '^Signature=adhoc$' <<<"$info" || fail "$target is not signed ad-hoc"
        ! grep -qE '^CodeDirectory .*flags=.*runtime' <<<"$info" || fail "$target has the hardened runtime"
        echo "$(basename "$target"): $(grep -E '^(Identifier|CodeDirectory|Signature)' <<<"$info" | tr '\n' ' ')"
    done
    ok "signed ad-hoc, no hardened runtime"

    # Developer commands (src/app/Main.cpp, FIXOMIC_DEV_COMMANDS) are compiled out of Release.
    local options strings_out found=""
    options="$(grep -oE 'arg == "--[a-z-]+"' "$ROOT/src/app/Main.cpp" | sed -E 's/.*"(--[a-z-]+)"/\1/' | sort -u)"
    [[ -n "$options" ]] || fail "no developer commands found in src/app/Main.cpp"
    strings_out="$(strings -a "$bin")"
    for option in $options; do
        if grep -qF -- "$option" <<<"$strings_out"; then found+=" $option"; fi
    done
    [[ -z "$found" ]] || fail "developer commands in the Release binary:$found"
    grep -qF -- "scan-worker" <<<"$strings_out" || fail "the scan worker id is missing from the binary"
    ok "none of the $(grep -c . <<<"$options") developer commands in the binary; scan-worker present"
}

summary() {
    step "done"
    ls -l "$ZIP" "$ZIP.sha256"
    echo "size: $(du -h "$ZIP" | cut -f1) ($(stat -f %z "$ZIP") bytes), unpacked $(du -sh "$CHECK_DIR/fixomic.app" | cut -f1)"
    cat "$ZIP.sha256"
    echo "timings:"
    printf '  %s\n' "${TIMINGS[@]}"
}

start=$SECONDS
if [[ $CLEAN -eq 1 ]]; then
    echo "removing $REL"
    rm -rf "$REL"
fi
timed "dependencies + driver" deps
timed "configure" configure
timed "build" build
timed "sign + zip" package
timed "checks" checks
TIMINGS+=("$(printf '%-24s %5d s' total $((SECONDS - start)))")
summary
