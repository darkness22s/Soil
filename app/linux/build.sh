#!/usr/bin/env bash
# Create a relocatable Linux tarball with the real app.html, skill, and Python host.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEST="$ROOT/app/linux/dist"
BUNDLE="$DEST/Soil-Linux"
rm -rf "$BUNDLE"
mkdir -p "$BUNDLE/ui" "$BUNDLE/skill/soil-control" "$BUNDLE/third-party"
install -m 755 "$ROOT/app/linux/soil.py" "$BUNDLE/soil.py"
install -m 644 "$ROOT/app/linux/computer_use.py" "$BUNDLE/computer_use.py"
install -m 644 "$ROOT/app/linux/requirements-computer-use.txt" "$BUNDLE/requirements-computer-use.txt"
install -m 644 "$ROOT/app/linux/THIRD_PARTY_NOTICES.md" "$BUNDLE/THIRD_PARTY_NOTICES.md"
install -m 644 "$ROOT/app/linux/third-party/vncdotool-LICENSE.txt" "$BUNDLE/third-party/vncdotool-LICENSE.txt"
# Bundle the upstream VNC automation library and wheels, not merely a URL or
# an install-on-first-use command. No package work occurs on app/VM startup.
python3 -m pip install --disable-pip-version-check --no-input --no-cache-dir \
    --target "$BUNDLE/vendor" -r "$ROOT/app/linux/requirements-computer-use.txt"
test -f "$BUNDLE/vendor/vncdotool/command.py"
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
