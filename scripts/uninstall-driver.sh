#!/usr/bin/env bash
# Removes /Library/Audio/Plug-Ins/HAL/fixomic.driver and restarts coreaudiod.
# Run as root: sudo scripts/uninstall-driver.sh. Other drivers in that folder are left untouched.
set -euo pipefail

DEST="/Library/Audio/Plug-Ins/HAL/fixomic.driver"

[[ $EUID -eq 0 ]] || { echo "error: run with sudo" >&2; exit 1; }
if [[ -e "$DEST" ]]; then
    rm -rf "$DEST"
    killall -9 coreaudiod 2>/dev/null || true
    echo "removed $DEST; coreaudiod restarted"
else
    echo "$DEST not installed"
fi
