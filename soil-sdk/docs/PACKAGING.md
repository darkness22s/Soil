# Packaging guide

## Requirements

* Python 3.8+ on the build host (Windows, macOS, Linux). No external Python
  packages are required; `cryptography` is optional and only needed for signing.
* The SDK's `tools/soilmk.py` + `tools/mksquashfs.py` (self-contained squashfs
  writer — no `mksquashfs` binary needed).

## Quick start

```sh
python tools/soil.py init my-app
python tools/soil.py dev my-app
python tools/soil.py build my-app
```

For React + TypeScript or plain Vite:

```sh
python tools/soil.py init --template react-ts my-app
cd my-app
npm install
python /path/to/soil-sdk/tools/soil.py dev .
python /path/to/soil-sdk/tools/soil.py build .
```

The scaffold's `soil.json` stores `build.command` and `build.webDir`, so
`soil build .` runs the frontend build and packs its output. To use another
frontend builder:

```sh
python /path/to/soil-sdk/tools/soil.py build . --frontend-build "npm run build" --web-dir dist
```

The low-level packer remains available when you need direct control:

```sh
python tools/soilmk.py new my-app
cd my-app
python tools/soilmk.py pack . -o MyApp.sol
python tools/soilmk.py verify MyApp.sol
```

Signing:

```sh
python tools/soil.py keygen soil-sign
python tools/soil.py build . --sign soil-sign.key.json
python tools/soil.py verify MyApp.sol --key soil-sign.pub.pem
```

Trusting a key inside Soil:

```sh
soil trust soil-sign.pub.pem           # copies to ~/.config/soil/trust/
```

## App directory layout

```
my-app/
  soil.json            manifest (required)
  icon.png             PNG icon, 128x128 recommended
  web/
    index.html         entry
    app.js
    style.css
  bin/                 optional native helpers (kind webapp+native)
  install.sh           optional (kind installer)
```

Everything in the directory is packed (except `.git`, `node_modules`, `dist`,
`*.sol` at the root). Hidden files are included; delete build junk before
packing. With `build.webDir` or `--web-dir`, only the staged frontend output is
packed.

## Packing details

1. Validate `soil.json` (schema, ids, paths, kinds, permissions).
2. Reject symlinks/hardlinks, absolute paths, `..`, and files > 64 MiB each.
3. Copy the manifest to `/manifest.json` inside the payload.
4. Build a gzip squashfs image (`mksquashfs.py`, block size 128 KiB).
5. Write the 128-byte header, then the manifest JSON padded with spaces so the
   payload begins on a 512-byte boundary.
6. Compute `sha256(padded_manifest || payload)` and store it in the header.
7. If `--sign KEY` is given, sign the digest (Ed25519) and store the signature.

`soilmk pack` prints the resulting header summary and package hash.

## Reproducibility

Packing is deterministic for identical inputs, except the squashfs inode mtime
fields (set to a fixed value) — building the same directory twice yields the
same payload bytes on the same SDK version.

## Host testing without Soil

`soilmk verify` checks format, bounds, digest and (optionally) signature against
a local key. A full runtime test needs Soil (or any Linux with WebKitGTK 4.1);
see `docs/TESTING.md`.

## Versioning your app

* `1.0.0` → `1.0.1` for fixes. `soil install` replaces the previous package with
  the same id.
* Keep the id stable; it owns the data dir, desktop entry and icon.

## Troubleshooting

| Symptom | Cause |
|---------|-------|
| `manifest: entry missing` | `entry` path not present in the app dir |
| `web output directory not found` | run the frontend build or fix `build.webDir` |
| `frontend build failed` | fix the configured build command; rerun `soil build` |
| `manifest: exec path not found` | listed helper does not exist |
| `pack: symlink rejected` | remove/replace symlinks in the app dir |
| `verify: digest mismatch` | file corrupted or truncated; repack |
| `verify: signature invalid` | header changed after signing; repack+resign |
