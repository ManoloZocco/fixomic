#!/usr/bin/env bash
# Installs build/driver/fixomic.driver into /Library/Audio/Plug-Ins/HAL and restarts coreaudiod.
# Run as root: sudo scripts/install-driver.sh. Other drivers in that folder are left untouched.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/build/driver/fixomic.driver"
HAL_DIR="/Library/Audio/Plug-Ins/HAL"
DEST="$HAL_DIR/fixomic.driver"

[[ $EUID -eq 0 ]] || { echo "error: run with sudo" >&2; exit 1; }
[[ -d "$SRC" ]] || { echo "error: $SRC not found (run scripts/build-driver.sh first)" >&2; exit 1; }
codesign --verify --deep --strict "$SRC"

mkdir -p "$HAL_DIR"
rm -rf "$DEST"
ditto "$SRC" "$DEST"
chown -R root:wheel "$DEST"
chmod -R go-w "$DEST"

# coreaudiod is respawned by launchd and rescans the HAL plug-ins folder.
killall -9 coreaudiod 2>/dev/null || true
echo "installed $DEST; coreaudiod restarted"
