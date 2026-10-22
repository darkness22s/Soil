---
name: soil-app-builder
description: Build, package and sign single-file Soil web apps (.sol) with the Soil SDK. Load when creating or modifying a Soil app, writing its soil.json manifest, using the window.soil bridge (store, dialogs, clipboard, notify, exec), signing packages, or debugging packaging/runtime issues.
---

# Soil App Builder

Build a `.sol` app: a signed squashfs payload + JSON manifest rendered by the
Soil WebKitGTK runtime. No Node or Chromium is shipped in the app.

## Prerequisites

* Python 3.8+ (only `cryptography` for signing). Node/npm only if using the
  optional Vite template.
* `soil-sdk/tools/soil.py` (scaffold/dev/build/run) and
  `soil-sdk/tools/soilmk.py` (low-level pack/sign/verify); `docs/` for reference.
* A Soil system, or any Linux with GTK3 + WebKitGTK 4.1, to test the runtime.

## Workflow

1. **Scaffold and preview**

   ```sh
   python tools/soil.py init my-app
   python tools/soil.py dev my-app
   ```

2. **Edit `soil.json`** (schema: `schema/soil.schema.json`)

   Required: `schema: 1`, `id` (`^[a-z0-9][a-z0-9._-]{1,63}$`), `name`, semver
   `version`, `kind` (`webapp` | `webapp+native` | `installer`), `entry` (except
   installer), `icon`.

   Set only the permissions you use. Each ungranted call rejects.

   ```json
   {
     "schema": 1,
     "id": "org.example.notes",
     "name": "Notes",
     "version": "1.0.0",
     "kind": "webapp",
     "entry": "web/index.html",
     "icon": "icon.png",
     "window": { "width": 1000, "height": 700, "resizable": true },
     "permissions": ["clipboard", "notifications", "dialogs"],
     "min_ram_mb": 96
   }
   ```

3. **Write the UI.** Everything under the app dir is packed (`.git`,
   `node_modules`, `dist` and root `*.sol` are excluded). Use relative URLs:
   the entry is served as `soil://app/<entry>`. Vendor JS locally — do not rely
   on CDNs at runtime.

   ```html
   <!doctype html><meta charset="utf-8">
   <title>Notes</title>
   <textarea id="body"></textarea><button id="save">Save</button>
   <script>
     (async () => {
       const {id} = await soil.app.info();
       save.onclick = async () => {
         await soil.store.set("note", body.value);
         await soil.notify("Saved", id);
       };
     })();
   </script>
   ```

4. **Package and sign**

   ```sh
   python tools/soil.py keygen my-key            # my-key.key.json + my-key.pub.pem
   python tools/soil.py build . --sign my-key.key.json
   python tools/soil.py verify Notes.sol --key my-key.pub.pem
   ```

   For Vite/React-style output, scaffold with `--template vite` (it injects the
   dev bridge) and stage the normal build:

   ```sh
   python tools/soil.py init --template vite my-app
   cd my-app && npm install
   python /path/to/soil-sdk/tools/soil.py dev . --frontend "npm run dev"
   npm run build
   python /path/to/soil-sdk/tools/soil.py build . --web-dir dist
   ```

   `build` validates paths, rejects symlinks/hardlinks/`..`, copies the manifest
   into the payload, pads the header manifest to 512 bytes and writes the digest
   and Ed25519 signature. `soilmk.py` remains available for direct control.

5. **Test on Soil**

   ```sh
   soil trust my-key.pub.pem
   soil run Notes.sol            # or: soil install Notes.sol && soil launch org.example.notes
   soil verify Notes.sol
   SOIL_DEBUG=1 soil run Notes.sol   # WebKit inspector + console output
   ```

## Bridge API (`window.soil`)

All calls return promises and reject with `Error(message)`. Inject before any
app script; page `document.title` is pushed to the window title automatically.

| Area | API |
|------|-----|
| App | `soil.app.info()` → `{id,name,version,kind,dataDir}` |
| Window | `soil.window.setTitle(s)`, `.close()`, `.setSize(w,h)`, `.minimize()` |
| Dialogs (perm `dialogs`) | `soil.dialog.openFile({title,filters})` → `{path}`/`null`; `.saveFile({title,defaultName})` |
| Clipboard (perm `clipboard`) | `soil.clipboard.read()` → `{text}`; `.write(text)` |
| Shell (perm `openExternal`) | `soil.shell.openExternal(url)` — http/https/mailto only |
| Notify (perm `notifications`) | `soil.notify(title, body)` |
| Store | `soil.store.get(key)` → `{value}`; `.set(key,value)`; `.delete(key)`; `.keys()` |
| Exec (perm `exec`) | `soil.exec.run(path, argsArray)` → `{pid}` |

Details and examples: `docs/RUNTIME-API.md`.

## Runtime guarantees and limits

* `soil://app/` serves the mounted payload; unknown files → 404; traversal and
  symlink escapes are rejected.
* The bridge is only injected in the app webview; no shell execution.
* `exec` paths must be listed in `manifest.exec` and live inside the payload
  (kind `webapp+native` or `installer`). Env: `SOIL_APP_ID`, `SOIL_VERSION`,
  `SOIL_APPDIR`, `SOIL_DATA`; cwd = app root.
* Store values are strings, persisted to `<dataDir>/store.json`; `/home` is
  tmpfs in the default image, so data is session-scoped.
* One window per instance; `alert`/`confirm` are not implemented.
* `min_ram_mb` is advisory; first WebKit paint can take 10–30 s at 200 MB RAM.

## Pitfalls

| Symptom | Fix |
|---------|-----|
| fetch of local JSON fails / opaque origin | Use relative URLs; do not assume `file://` or cross-origin semantics |
| `permission denied: dialogs` etc. | Add the permission to `soil.json` |
| `entry missing` / `exec path not found` | Paths are relative to the app dir; fix and repack |
| Assets 404 | Keep URLs relative to the entry; check case-sensitive names |
| Giant package | Remove `node_modules`, build caches and unused media before packing |
| `signature invalid` | Header changed after signing; repack then sign (or `soilmk sign`) |

## Done checklist

* `soilmk pack` succeeds and `soilmk verify` reports digest OK.
* All used bridge calls are covered by permissions.
* App launches in Soil, title matches, store survives relaunch within session.
* No console errors with `SOIL_DEBUG=1`.
* Package signed with the intended key; public key delivered to users.
