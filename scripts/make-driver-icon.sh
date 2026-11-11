#!/usr/bin/env bash
# Regenerates the placeholder device icon driver/resources/fixomic.icns (committed; only needed
# when the icon changes). Draws a 1024 px PNG with AppKit, scales it with sips, packs with iconutil.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/driver/resources/fixomic.icns"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

cat > "$WORK/draw.swift" <<'SWIFT'
import AppKit
let size = 1024.0
let image = NSImage(size: NSSize(width: size, height: size))
image.lockFocus()
let rect = NSRect(x: 80, y: 80, width: size - 160, height: size - 160)
NSColor(calibratedRed: 0.11, green: 0.42, blue: 0.86, alpha: 1).setFill()
NSBezierPath(roundedRect: rect, xRadius: 180, yRadius: 180).fill()
let text = "fx" as NSString
let attributes: [NSAttributedString.Key: Any] = [
    .font: NSFont.systemFont(ofSize: 460, weight: .bold),
    .foregroundColor: NSColor.white,
]
let textSize = text.size(withAttributes: attributes)
text.draw(at: NSPoint(x: (size - textSize.width) / 2, y: (size - textSize.height) / 2), withAttributes: attributes)
image.unlockFocus()
let bitmap = NSBitmapImageRep(cgImage: image.cgImage(forProposedRect: nil, context: nil, hints: nil)!)
try! bitmap.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: CommandLine.arguments[1]))
SWIFT
swift "$WORK/draw.swift" "$WORK/icon_1024.png"

ICONSET="$WORK/fixomic.iconset"
mkdir -p "$ICONSET"
for s in 16 32 128 256 512; do
    sips -z "$s" "$s" "$WORK/icon_1024.png" --out "$ICONSET/icon_${s}x${s}.png" >/dev/null
    d=$((s * 2))
    sips -z "$d" "$d" "$WORK/icon_1024.png" --out "$ICONSET/icon_${s}x${s}@2x.png" >/dev/null
done
mkdir -p "$(dirname "$OUT")"
iconutil -c icns "$ICONSET" -o "$OUT"
echo "wrote $OUT"
