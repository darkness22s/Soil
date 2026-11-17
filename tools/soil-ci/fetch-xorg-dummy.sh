#!/usr/bin/env bash
# Build-time only: fetch the MIT/X11 licensed Xorg dummy video module
# proven with the Tiny Core Xorg ABI. Never downloads on the user's VM.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
out="${1:-$root/tools/soil-ci/out}"
temp="$(mktemp -d -t soil-xorg-dummy-XXXXXX)"
trap 'rm -rf "$temp"' EXIT
cd "$temp"
apt-get download "xserver-xorg-video-dummy=1:0.4.0-1ubuntu1"
package="$(find "$temp" -maxdepth 1 -name '*.deb' -print -quit)"
[[ -n "$package" ]] || { echo "Pinned Xorg dummy package unavailable" >&2; exit 1; }
version="$(dpkg-deb -f "$package" Version)"
[[ "$version" == "1:0.4.0-1ubuntu1" ]] || { echo "Xorg module package version mismatch: $version" >&2; exit 1; }
dpkg-deb -x "$package" unpack
module="$(find "$temp/unpack" -name dummy_drv.so -type f -print -quit)"
notice="$(find "$temp/unpack" -path '*/xserver-xorg-video-dummy/copyright' -type f -print -quit)"
[[ -n "$module" && -n "$notice" ]] || { echo "Missing module or license notice in pinned package" >&2; exit 1; }
python3 - "$module" <<'PY'
from pathlib import Path
import sys
elf = Path(sys.argv[1]).read_bytes()
if not (1024 <= len(elf) <= 1024*1024 and
        elf[:6] == b'\x7fELF\x02\x01' and elf[18:20] == b'\x3e\x00'):
    raise SystemExit('Xorg dummy module is not a Linux x86_64 ELF.')
PY
mkdir -p "$out"
cp "$module" "$out/dummy_drv.so"
cp "$notice" "$out/xf86-video-dummy-LICENSE.txt"
echo "Pinned Xorg dummy driver $version: $(wc -c < "$out/dummy_drv.so") bytes"
sha256sum "$package" "$out/dummy_drv.so"
