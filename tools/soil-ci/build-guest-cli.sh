#!/usr/bin/env bash
# Build a portable, static Linux x86_64 Soil .sol CLI for the bundled Tiny Core guest.
# This runs on an Ubuntu builder, not on the Windows user's machine.
set -euo pipefail
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
output="${1:-$repo/tools/soil-ci/out/soil}"
command -v musl-gcc >/dev/null || { echo "Install musl-tools first (musl-gcc missing)" >&2; exit 1; }
mkdir -p "$(dirname "$output")"
musl-gcc -static -DSOIL_STATIC_MUSL -O2 -Wall -Wextra -Wno-format-truncation \
  -o "$output" \
  "$repo/soil-sdk/runtime/soil.c" \
  "$repo/soil-sdk/runtime/sol.c" \
  "$repo/soil-sdk/runtime/json.c" \
  "$repo/soil-sdk/runtime/sha256.c"
chmod 755 "$output"
python3 - "$repo/tools/soil-ci/inject-native-guest.py" "$output" <<'PY'
import importlib.util
import pathlib
import sys
spec = importlib.util.spec_from_file_location("inject_guest", sys.argv[1])
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
module.validate_runtime(pathlib.Path(sys.argv[2]).read_bytes())
print("Validated static x86_64 ELF:", sys.argv[2])
PY
"$output" help | grep -q 'soil run'
echo "Static Soil SDK guest CLI built: $output"
