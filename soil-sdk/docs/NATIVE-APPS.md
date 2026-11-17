# Packaging native Linux applications

Soil can distribute real Linux x86_64 applications in the existing signed
`.sol` format. This is a **native-only package** (kind 4), not a web app,
and does not start WebKitGTK.

## Quick start: ELF or AppImage

```sh
python soil-sdk/tools/soil.py native ./Editor.AppImage \
  --id org.example.editor --name Editor --version 1.0.0 -o Editor.sol

python soil-sdk/tools/soil.py verify Editor.sol
# Inside a running Soil Linux desktop, with access to the file:
soil run Editor.sol
soil install Editor.sol
soil launch org.example.editor
```

The CLI checks the native ELF header for **Linux x86_64**, generates an icon
unless `--icon image.png` is supplied, signs with `--sign key.json` if
requested, and verifies the resulting SHA-256 digest. The command also accepts
executable shebang scripts. Packages may contain individual files up to
256 MiB (512 MiB total uncompressed payload).

## Package an application directory with resources and private libraries

A prepared app directory (such as an AppDir **after resolving symlinks**) can
include its assets and private libraries. Set `--entry` to an executable
relative to the directory. `AppRun` is the default:

```text
Editor.AppDir/
  AppRun                 # executable ELF or shebang shell launcher
  usr/bin/editor
  usr/lib/               # libraries supplied by the publisher
  usr/share/             # icons, translations, other assets
```

```sh
python soil-sdk/tools/soil.py native Editor.AppDir --entry AppRun \
  --icon editor.png --terminal --id org.example.editor -o Editor.sol
```

The AppRun script can use `APPDIR` (read-only package mount) to find the
resources and `SOIL_DATA` (writable data path) for persistent files during
the session. `SOIL_APP_ID` and `SOIL_APPDIR` are also set.

Native `soil.json` manifest if building with the low-level packer:

```json
{
  "schema": 1,
  "id": "org.example.editor",
  "name": "Editor",
  "version": "1.0.0",
  "kind": "native",
  "entry": "app/AppRun",
  "icon": "icon.png",
  "args": ["--safe-mode"],
  "terminal": false,
  "permissions": []
}
```

Native launch from `soil run` forwards additional CLI arguments after the
package filename, after the default `args`. `soil install` registers a
desktop entry, and `soil remove` removes it. The payload stays mounted
read-only while the native process is running; no host execution occurs on
Windows. AppImage Type 2 uses `APPIMAGE_EXTRACT_AND_RUN=1` to work without FUSE.

## Compatibility and security

**Linux-based is not sufficient for universal binary compatibility.**

* The packaged executable must be Linux **x86_64**. ARM64, Windows EXE,
  Windows MSI, ISO boot media, `.deb`, and `.rpm` files are not executable
  entries and are not supported by `soil native`.
* Dynamic ELF programs must find the correct runtime loader and shared
  libraries. Soil's compact Tiny Core guest is **not** equivalent to Debian or
  Ubuntu. An app dynamically linked to a different glibc/loader, depending on
  missing GTK/Qt libraries, or expecting system services can still fail.
* Bundle redistributable private dependencies with the app and use an
  `AppRun` script to set `LD_LIBRARY_PATH` if appropriate. This cannot
  fix an incompatible ELF interpreter embedded in the binary.
* AppDir trees may contain symlinks. The v1 SquashFS packer rejects symlinks
  for safety. Resolve in-tree symlinks to regular files before packaging;
  external links cannot be included.
* Treat imported executables as untrusted code. They run with the **guest
  user's** privileges and can access guest files. Use signing and strict
  verification for trusted distribution; this is not an application sandbox.
* On the stock guest, user-home installs and app data live in tmpfs, so stop
  or reboot loses them unless persistent storage is configured.

## What to test before publishing

1. Pack the actual release ELF/AppImage, not only a placeholder.
2. Run `python soil-sdk/tools/soil.py verify Editor.sol` and inspect
   `python soil-sdk/tools/soilmk.py inspect Editor.sol`.
3. Boot the default Soil Linux guest and run `soil run Editor.sol`, then
   `soil install Editor.sol && soil launch org.example.editor`.
4. Exercise the GUI/CLI, open files, test write access via `SOIL_DATA`,
   close, relaunch and remove the app; check stderr for missing libraries.
5. Check the same package on every actual guest image and architecture you
   claim to support.

The host-side CI checks the package builder, digest, header, manifest and C
runtime parser. It **does not** prove full third-party graphical execution
inside a booted VM; use a real guest smoke runner for release acceptance.
