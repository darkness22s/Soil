# Soil SDK 0.1.0 — release notes

First complete cut of the Soil app framework and SDK.

## Highlights

* **`.sol` package format v1** — 128-byte header (`S0L1`), JSON manifest padded
  to a 512-byte boundary, gzip squashfs payload, SHA-256 digest and optional
  Ed25519 signature. Readers enforce alignment and reject trailing bytes.
* **`soil.py` developer CLI** — scaffold, localhost preview with a `window.soil`
  shim and live reload, build/verify, runtime handoff, plus `--web-dir` staging
  for Vite/React-style output.
* **Vite workflow** — `soil init --template vite` adds a dev-only bridge
  injection plugin; `soil dev --frontend "npm run dev"` starts Vite, waits for
  it, and keeps HMR. Built output is packaged with `--web-dir dist`.
* **`soilmk` SDK tool** — low-level pack, sign, verify and keygen in pure Python;
  deterministic payloads; no native mksquashfs required.
* **`soil` CLI** — run, install, launch, list, remove, info, verify, trust,
  untrust, docs. Read-only loop mounts with automatic cleanup, sudo helper,
  installer hooks, desktop entries and icons.
* **WebKitGTK runtime** — `soil://app/` scheme, injected `window.soil` bridge
  (app info, window controls, dialogs, clipboard, notifications, external links,
  key/value store, whitelisted exec), title sync, transactional writes, strict
  path containment. Memory use ~60–70 MB RSS for the sample apps.
* **Trust store** — system (`/usr/local/share/soil/trust`) and user
  (`~/.config/soil/trust`) keys; untrusted packages warn, `SOIL_STRICT=1`
  refuses.
* **Desktop integration** — Fluxbox menu entry, pcmanfm desktop icon, `.sol`
  MIME type and default handler. The kernel lacks `binfmt_misc`, so launching is
  via `soil run` / MIME association rather than direct exec.
* **Samples** — Hello (bridge demo), Frontend (React dashboard), DemoInstaller
  (installer-kind hook), all signed with the demo key.
* **Docs** — format, manifest, packaging, runtime API, security, install/run,
  testing, plus a JSON schema and two AI-agent skills.

## Verified on the reference image

* Package verify: digest OK, signature trusted, tamper detection (exit 2).
* Install / launch / remove, bridge store counter, installer hook output.
* Rendering of Hello and Frontend at 200 MB guest RAM; window title sync.
* MIME caches baked with `application/x-soil-package=soil-package.desktop`;
  default handler set to `soil run %f`.
* Host unit tests: SHA-256 vectors, JSON parse/write/escape, header and
  signature verification.

## Known limitations

* No `binfmt_misc` in the stock kernel: `.sol` files are not directly executable.
* Installs and app data persist only for the session (`/home` is tmpfs).
* Single window per instance; no JavaScript dialogs; DevTools opt-in via
  `SOIL_DEBUG=1`.
* `min_ram_mb` is advisory only.
