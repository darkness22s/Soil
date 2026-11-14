#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
APP="$ROOT/dist/Soil.app"
CONTENTS="$APP/Contents"
MACOS="$CONTENTS/MacOS"
RESOURCES="$CONTENTS/Resources"

command -v swift >/dev/null 2>&1 || {
    echo "Swift 5.9+ and Xcode Command Line Tools are required." >&2
    exit 1
}

cd "$ROOT"
swift build --configuration release
BIN_DIR=$(swift build --configuration release --show-bin-path)
mkdir -p "$MACOS" "$RESOURCES"
cp "$BIN_DIR/SoilMac" "$MACOS/SoilMac"
cat > "$CONTENTS/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>SoilMac</string>
    <key>CFBundleIdentifier</key><string>com.soil.desktop.macos</string>
    <key>CFBundleName</key><string>Soil</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>0.1.0</string>
    <key>LSMinimumSystemVersion</key><string>13.0</string>
    <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST

echo "Created $APP"
echo "Install QEMU with: brew install qemu"
echo "Place vmlinuz64, core2.gz, and soil.squashfs in ~/Library/Application Support/Soil/images/"
