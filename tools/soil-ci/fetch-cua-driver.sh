#!/usr/bin/env bash
# Download an exact upstream Cua Driver release only during packaging.
# Nothing is fetched when an end user's Soil VM starts or when an agent uses it.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
out="${1:-$root/tools/soil-ci/out/cua-driver}"
archive="$(mktemp -t soil-cua-XXXXXX.tar.gz)"
folder="$(mktemp -d -t soil-cua-XXXXXX)"
trap 'rm -rf "$folder" "$archive"' EXIT

url="https://github.com/trycua/cua/releases/download/cua-driver-rs-v0.49.5/cua-driver-rs-0.49.5-linux-x86_64-binary.tar.gz"
sha256="e01fe9dd44e6a7d2d8ac128737fb0204c6deb57929da6cc2871c049eda96f7dc"
curl --fail --location --retry 3 --silent --show-error "$url" -o "$archive"
echo "$sha256  $archive" | sha256sum --check -
tar -xzf "$archive" -C "$folder"
binary="$(find "$folder" -type f -name cua-driver -print -quit)"
[[ -n "$binary" && -f "$binary" ]] || { echo "Pinned Cua release is missing cua-driver" >&2; exit 1; }
python3 - "$binary" <<'PY'
import pathlib, struct, sys
b = pathlib.Path(sys.argv[1]).read_bytes()
if (not 65536 <= len(b) <= 150 * 1024 * 1024 or
        b[:6] != b"\x7fELF\x02\x01" or b[18:20] != b"\x3e\x00"):
    raise SystemExit("Upstream Cua Driver is not a Linux x86_64 ELF.")
PY
mkdir -p "$(dirname "$out")"
cp "$binary" "$out"
chmod 755 "$out"
echo "Pinned upstream Cua Driver v0.49.5 verified: $out ($(wc -c < "$out") bytes)"
