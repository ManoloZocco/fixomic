#!/usr/bin/env bash
# Builds webrtc-audio-processing (WebRTC AEC3) as static libraries for macOS:
#   third_party/install/webrtc-apm/{arm64,x86_64,universal}
# and copies the licence texts of the code compiled into them next to them:
#   third_party/install/webrtc-apm/licenses   (read by scripts/package-release.sh)
# Abseil is built from the meson wrap fallback and linked statically, so there is no runtime
# dependency on a Homebrew dylib. Requires: meson, ninja, git, Xcode command line tools.
#
#   scripts/build-webrtc-apm.sh                   full build (replaces the install)
#   scripts/build-webrtc-apm.sh --licenses-only   only (re)copies the licence texts, from the same
#                                                 source checkout, into an existing install
set -euo pipefail

APM_TAG="v2.1"
APM_URL="https://gitlab.freedesktop.org/pulseaudio/webrtc-audio-processing.git"
DEPLOYMENT_TARGET="14.4"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC_DIR="$ROOT/build/deps/webrtc-apm-src"
BUILD_ROOT="$ROOT/build/deps/webrtc-apm-build"
INSTALL_ROOT="$ROOT/third_party/install/webrtc-apm"

LICENSES_ONLY=0
case "${1:-}" in
    "") ;;
    --licenses-only) LICENSES_ONLY=1 ;;
    *) echo "usage: scripts/build-webrtc-apm.sh [--licenses-only]" >&2; exit 2 ;;
esac

fail() { echo "error: $*" >&2; exit 1; }

# The licence texts of everything compiled into the libraries (NOTICE lists them), from the source
# they were built from, under the names fixomic.app ships them with.
install_licenses() {
    local dest="$INSTALL_ROOT/licenses" abseil
    abseil="$(sed -nE 's/^directory *= *([^ ]+).*/\1/p' "$SRC_DIR/subprojects/abseil-cpp.wrap" 2>/dev/null || true)"
    [[ -n "$abseil" ]] || fail "cannot read the abseil directory from $SRC_DIR/subprojects/abseil-cpp.wrap"
    local files=(
        "$SRC_DIR/webrtc/LICENSE"                                       WebRTC-LICENSE.txt
        "$SRC_DIR/webrtc/PATENTS"                                       WebRTC-PATENTS.txt
        "$SRC_DIR/subprojects/$abseil/LICENSE"                          Abseil-LICENSE.txt
        "$SRC_DIR/webrtc/third_party/rnnoise/COPYING"                   RNNoise-COPYING.txt
        "$SRC_DIR/webrtc/third_party/pffft/LICENSE"                     PFFFT-LICENSE.txt
        "$SRC_DIR/webrtc/common_audio/third_party/ooura/LICENSE"        Ooura-FFT-LICENSE.txt
        "$SRC_DIR/webrtc/common_audio/third_party/spl_sqrt_floor/LICENSE" spl_sqrt_floor-LICENSE.txt
    )
    rm -rf "$dest"
    mkdir -p "$dest"
    local i
    for ((i = 0; i < ${#files[@]}; i += 2)); do
        [[ -f "${files[i]}" ]] || fail "licence file missing: ${files[i]}"
        cp "${files[i]}" "$dest/${files[i + 1]}"
    done
    echo "licences: $((${#files[@]} / 2)) files in $dest"
}

for tool in meson ninja git lipo clang clang++; do
    command -v "$tool" >/dev/null || { echo "error: '$tool' not found (brew install meson ninja)" >&2; exit 1; }
done

if [[ ! -d "$SRC_DIR/.git" ]]; then
    mkdir -p "$(dirname "$SRC_DIR")"
    git clone --depth 1 --branch "$APM_TAG" "$APM_URL" "$SRC_DIR"
fi

if [[ $LICENSES_ONLY -eq 1 ]]; then
    [[ -d "$INSTALL_ROOT/universal/lib" ]] || fail "$INSTALL_ROOT has no build yet: run scripts/build-webrtc-apm.sh"
    # Abseil's source (and its LICENSE) is unpacked by the meson setup of a full build: without it,
    # install_licenses fails and says which file is missing.
    install_licenses
    exit 0
fi

rm -rf "$INSTALL_ROOT"
mkdir -p "$INSTALL_ROOT"

write_cross_file() {
    local arch="$1" file="$2" cpu_family="$arch"
    local flags="'-arch', '$arch', '-mmacosx-version-min=$DEPLOYMENT_TARGET'"
    cat > "$file" <<CROSS
[binaries]
c = ['clang']
cpp = ['clang++']
ar = ['ar']
strip = ['strip']

[built-in options]
c_args = [$flags]
cpp_args = [$flags]
c_link_args = [$flags]
cpp_link_args = [$flags]

[host_machine]
system = 'darwin'
subsystem = 'macos'
kernel = 'xnu'
cpu_family = '$cpu_family'
cpu = '$arch'
endian = 'little'

[properties]
needs_exe_wrapper = true
CROSS
}

for arch in arm64 x86_64; do
    cross_file="$BUILD_ROOT/cross-$arch.ini"
    build_dir="$BUILD_ROOT/$arch"
    mkdir -p "$BUILD_ROOT"
    write_cross_file "$arch" "$cross_file"
    rm -rf "$build_dir"
    # The cross file is also used for the native arch so both slices get identical flags.
    meson setup "$build_dir" "$SRC_DIR" \
        --cross-file "$cross_file" \
        --prefix "$INSTALL_ROOT/$arch" \
        --libdir lib \
        --buildtype=release \
        -Ddefault_library=static \
        --force-fallback-for=abseil-cpp
    meson compile -C "$build_dir"
    meson install -C "$build_dir"
done

# Fat libraries: lipo every static library; headers/pkgconfig come from the arm64 install.
mkdir -p "$INSTALL_ROOT/universal/lib"
cp -R "$INSTALL_ROOT/arm64/include" "$INSTALL_ROOT/universal/include"
for lib in "$INSTALL_ROOT/arm64/lib/"*.a; do
    name="$(basename "$lib")"
    lipo -create "$INSTALL_ROOT/arm64/lib/$name" "$INSTALL_ROOT/x86_64/lib/$name" \
         -output "$INSTALL_ROOT/universal/lib/$name"
done

install_licenses

echo "Installed to $INSTALL_ROOT:"
lipo -info "$INSTALL_ROOT/universal/lib/libwebrtc-audio-processing-2.a"
