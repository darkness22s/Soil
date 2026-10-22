---
name: soil-integrator
description: Deploy, trust, install and bake Soil .sol packages into Soil images or running Soil systems. Load when moving packages into a Soil VM/image, managing the trust store, wiring MIME/desktop/menu integration, baking sample apps, or debugging loop-mount, signature or launch failures.
---

# Soil Integrator

Operate `.sol` packages on Soil: deliver them, trust publishers, install/launch,
and bake them into images. The stock kernel has **no binfmt_misc**, so `.sol`
files are not directly executable; always launch via `soil run`, the MIME
handler, the menu or desktop icons.

## Layout on a Soil system

| Path | Contents |
|------|----------|
| `/usr/local/bin/soil` | CLI |
| `/usr/local/bin/soil-app`, `/usr/local/lib/soil/soil-app-bin` | runtime wrapper + WebKit shell |
| `/usr/local/share/soil/trust/*.pem` | system trust keys |
| `~/.config/soil/trust/*.pem` | user trust keys |
| `/usr/local/share/soil/samples/` | baked sample `.sol` files |
| `~/.local/share/soil/pkg/` | installed packages (`<id>-<version>.sol`) |
| `~/.local/share/soil/{opt,icons,data}/` | extracted payloads, icons, app data |
| `~/.local/share/applications/soil-<id>.desktop` | installed launchers |

## Deliver and run a package

1. Copy the file into the guest. Options: attach a host share, serve over HTTP
   from the host (`wget http://10.0.2.2:PORT/App.sol` under the engine's slirp
   network), or base64 over SSH (`openssl enc -base64 -A -in App.sol`). BusyBox
   has no `base64` and no sftp-server, so `scp` will not work.
2. Verify and trust:

   ```sh
   soil verify App.sol                 # digest + signer
   soil trust publisher.pem            # per-user
   sudo soil trust publisher.pem       # system-wide (optional)
   SOIL_STRICT=1 soil run App.sol      # refuse untrusted signers
   ```

3. Install / launch / remove:

   ```sh
   soil install App.sol
   soil list
   soil launch org.example.app
   soil remove org.example.app
   ```

Installs live under `/home`, which is tmpfs in the default image: they last for
the session. Running from attached storage needs no install.

## Trust semantics

* Signature = Ed25519 over the 32-byte SHA-256 digest in the header.
* Unknown key → package is untrusted: warning on run, refusal with
  `SOIL_STRICT=1` / `soil run --strict`.
* Key names are the PEM file names (`publisher.pem`). Remove with
  `soil untrust publisher.pem`.
* Verify uses the `openssl` CLI (`pkeyutl -verify -rawin`), so `openssl` must be
  present in the guest.

## Baking into an image

Add to the image build (all paths under `/usr/local` unless noted):

1. `bin/soil`, `bin/soil-app`, `lib/soil/soil-app-bin` (mode 0755).
2. `share/soil/docs/*.md` + `soil.schema.json`.
3. Sample `.sol` files in `share/soil/samples/` (0755).
4. Publisher public keys in `share/soil/trust/` (`.pem`, 0644).
5. Desktop entries in `share/applications/`. **Exec must be
   `soil run /path/App.sol`** — a bare `.sol` path fails without binfmt_misc.
6. Fluxbox menu entry: `[exec] (App) {soil run /path/App.sol}`.
7. MIME integration:
   * `share/mime/packages/soil.xml` (`application/x-soil-package`, `*.sol`,
     magic `S0L1` at offset 0)
   * `share/applications/soil-package.desktop` (`Exec=soil run %f`, `NoDisplay=true`)
   * `share/applications/mimeapps.list` with
     `application/x-soil-package=soil-package.desktop`

   The rootfs is read-only at boot, so `update-mime-database` cannot run at boot.
   Generate `share/mime/mime.cache` and `share/applications/mimeinfo.cache` on a
   writable copy and bake them **after** any boot-delta/cache overlays:

   ```sh
   cp -a /usr/local/share/mime /tmp/mime
   cp -a /usr/local/share/applications /tmp/apps
   update-mime-database /tmp/mime
   update-desktop-database /tmp/apps
   grep -a x-soil-package /tmp/mime/mime.cache
   grep soil /tmp/apps/mimeinfo.cache
   ```

8. Loop device nodes: create `/dev/loop-control` (c 10 237) and `/dev/loopN`
   (b 7 N) in bootlocal if udev is absent; the CLI also creates nodes on demand.

## Troubleshooting

| Symptom | Cause / fix |
|---------|-------------|
| `syntax error` when running `./App.sol` | No binfmt_misc in kernel — use `soil run App.sol` |
| `soil: no free loop device` | Missing `/dev/loop-control` / loop nodes; mknod them or let soil create them |
| mount returns `Input/output error` | Loop fd closed before mount (AUTOCLEAR) — use a runtime that keeps it open |
| mount denied | `tc` needs passwordless sudo for `soil _mount/_umount`; launch as the desktop user |
| `payload offset not sector-aligned` | Package built by an old tool; repack with current `soilmk` |
| `digest: MISMATCH` | File corrupted/truncated or modified; repack |
| `signature: invalid` | Header modified after signing; repack and re-sign |
| `untrusted package` | Install the publisher PEM in a trust dir or use `SOIL_STRICT=0` |
| Blank/slow window at 200 MB | First WebKit paint is slow; wait 20–30 s or raise RAM |
| MIME type unknown at boot | Baked `mime.cache` missing/stale — regenerate and rebake after boot delta |
| Desktop entry does nothing | `Exec` points at the `.sol` directly — change to `soil run ...` |

## Verification checklist

* `soil verify` → digest OK, signature trusted.
* `soil run` shows the app with title sync; `store.json` exists under data dir.
* `soil install` creates desktop entry + `~/.local/share/soil/pkg` copy;
  `soil remove` cleans both.
* Tampered copy reports `digest: MISMATCH` (exit 2).
* After closing the app, `mount | grep soil` and `losetup -a` show no leftovers.
* `grep -a x-soil-package /usr/local/share/mime/mime.cache` finds the type and
  `mimeinfo.cache` maps it to `soil-package.desktop`.

## Reference

* Format: `docs/SOL-FORMAT.md`; packaging: `docs/PACKAGING.md`; runtime:
  `docs/RUNTIME-API.md`; testing: `docs/TESTING.md`.
* Reference image build: `remaster.py` (not shipped) bakes the same layout.
