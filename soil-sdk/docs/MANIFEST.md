# `soil.json` manifest

Every app directory contains `soil.json` at its root. `soilmk pack` validates it,
copies it into the payload as `/manifest.json`, and embeds it in the `.sol` header.

## Example

```json
{
  "schema": 1,
  "id": "org.soil.hello",
  "name": "Hello Soil",
  "version": "1.0.0",
  "kind": "webapp",
  "description": "Minimal Soil app that exercises the runtime bridge.",
  "author": "Soil",
  "homepage": "https://example.invalid/hello",
  "entry": "web/index.html",
  "icon": "icon.png",
  "window": { "width": 900, "height": 640, "resizable": true },
  "permissions": ["clipboard", "dialogs", "notifications", "openExternal"],
  "exec": [],
  "install": null,
  "min_ram_mb": 96,
  "build": {
    "command": "npm run build",
    "webDir": "dist",
    "entry": "index.html"
  },
  "dev": {
    "command": "npm run dev",
    "url": "http://127.0.0.1:5173/"
  }
}
```

## Fields

| Field | Type | Required | Meaning |
|-------|------|----------|---------|
| `schema` | int | yes | Manifest schema version, currently `1`. |
| `id` | string | yes | Unique package id, `^[a-z0-9][a-z0-9._-]{1,63}$`. Used for paths and desktop entries. |
| `name` | string | yes | Display name (window title, desktop launcher). |
| `version` | string | yes | `MAJOR.MINOR.PATCH`. Installing the same id replaces the previous file. |
| `kind` | string | yes | `webapp`, `installer`, `webapp+native`, or `native`. Must match header kind. |
| `entry` | string | webapp / webapp+native / native | HTML entry for web kinds; Linux executable or script for native, relative path inside payload. |
| `icon` | string | yes | PNG path inside payload. 128×128 recommended. |
| `description` | string | no | One-line description. |
| `author` | string | no | Author or publisher string. |
| `homepage` | string | no | Informational URL. |
| `window` | object | no | `width` (int), `height` (int), `resizable` (bool). Defaults 900×640, resizable. |
| `permissions` | array | no | Runtime capabilities, see below. Default `[]`. |
| `exec` | array | kind 3 / installer | Relative paths of bundled executables allowed via `soil.exec`. |
| `install` | object | kind `installer` | `{ "command": "install.sh", "args": [], "root": false }`. |
| `args` | array | no | Default arguments passed to a `native` executable (maximum 32 strings). |
| `terminal` | bool | no | Use `Terminal=true` for installed native desktop launchers. |
| `min_ram_mb` | int | no | Advisory; `soil run` warns when guest memory is below it. |
| `build` | object | no | Host-side frontend build: `command`, optional `webDir` and `entry`. |
| `dev` | object | no | Host-side dev server: `command` and optional `url`. |

## Frontend build and dev commands

`build` and `dev` are developer-tooling metadata. The runtime and installer
ignore them; only `soil.py` uses them on the development host.

```json
"build": {
  "command": "npm run build",
  "webDir": "dist",
  "entry": "index.html"
},
"dev": {
  "command": "npm run dev",
  "url": "http://127.0.0.1:5173/"
}
```

With this section, `soil dev .` starts `npm run dev`, and `soil build .` runs
`npm run build` then stages `dist` into the package's `web/` directory. Paths
must be relative and stay inside the app directory. CLI flags override the
manifest. Unknown fields are preserved but ignored by the runtime.

## Permissions

| Permission | Enables |
|------------|---------|
| `clipboard` | `soil.clipboard.read/write` |
| `dialogs` | `soil.dialog.openFile/saveFile` |
| `notifications` | `soil.notify` |
| `openExternal` | `soil.shell.openExternal` (opens the system browser) |
| `exec` | `soil.exec.run` for paths listed in `exec` |

Calls to ungranted capabilities reject with `{"error":"permission denied: <perm>"}`.

## Kinds

* **`webapp`** — HTML/CSS/JS UI served through `soil://app/`. No native helpers.
* **`webapp+native`** — same UI plus executables bundled in the payload and
  listed in `exec`; the UI can spawn them with `soil.exec.run`.
* **`native`** — a real Linux x86_64 ELF, Type 2 AppImage, or shebang
  launcher in the payload, referenced by `entry`. The runtime executes it
  directly, with `APPDIR`, `SOIL_APPDIR`, and writable `SOIL_DATA` set. This
  kind does not use WebKit or require an HTML frontend.
* **`installer`** — classic installer semantics: `soil install` extracts the
  payload to `~/.local/share/soil/opt/<id>/` and runs `install.command` (with
  `install.args`) from that directory. `install.root: true` runs the hook through
  `sudo -n`. The package is still launchable as a webapp if an `entry` is present.

## Constraints

* No absolute paths, no `..` segments in any path field.
* Symlinks and hardlinks in the app directory are rejected at pack time.
* `entry`, `icon`, `install.command` and every `exec` path must exist in the
  payload after packing.
* JSON must be UTF-8 without comments. Unknown fields are preserved but ignored.

Machine-readable schema: `schema/soil.schema.json`.
