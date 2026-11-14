#!/usr/bin/env bash
# Create a relocatable Linux tarball with the real app.html, skill, and Python host.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEST="$ROOT/app/linux/dist"
BUNDLE="$DEST/Soil-Linux"
rm -rf "$BUNDLE"
mkdir -p "$BUNDLE/ui" "$BUNDLE/skill/soil-control"
install -m 755 "$ROOT/app/linux/soil.py" "$BUNDLE/soil.py"
install -m 644 "$ROOT/app/linux/linux-ui.js" "$BUNDLE/linux-ui.js"
cp -a "$ROOT/app/Soil/ui/." "$BUNDLE/ui/"
install -m 644 "$ROOT/app/Soil/skill/soil-control/SKILL.md" "$BUNDLE/skill/soil-control/SKILL.md"
cat > "$BUNDLE/soil" <<'EOF'
#!/bin/sh
SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
exec python3 "$SCRIPT_DIR/soil.py" "$@"
EOF
chmod 755 "$BUNDLE/soil"
cat > "$BUNDLE/install.sh" <<'EOF'
#!/bin/sh
set -eu
SOURCE="$(dirname "$(readlink -f "$0")")"
TARGET="$HOME/.local/opt/soil"
mkdir -p "$HOME/.local/opt" "$HOME/.local/bin"
if [ "$SOURCE" != "$TARGET" ]; then
    mkdir -p "$TARGET"
    cp -a "$SOURCE/." "$TARGET/"
fi
ln -sfn "$TARGET/soil" "$HOME/.local/bin/soil"
printf 'Installed Soil to %s\nRun: %s (add ~/.local/bin to PATH if needed)\n' "$TARGET" "$HOME/.local/bin/soil"
EOF
chmod 755 "$BUNDLE/install.sh"
tar -czf "$DEST/Soil-Linux-x86_64.tar.gz" -C "$DEST" "Soil-Linux"
echo "Built $DEST/Soil-Linux-x86_64.tar.gz"
