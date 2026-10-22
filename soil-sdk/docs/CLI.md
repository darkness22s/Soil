# Soil developer CLI

`tools/soil.py` is the developer-facing CLI. It wraps the existing `.sol`
format, packer and runtime; it does not introduce a new package format.

## Quick start

```sh
python tools/soil.py init my-app
python tools/soil.py dev my-app
python tools/soil.py build my-app
```

`dev` serves the app on `127.0.0.1:5173`, injects a `window.soil` shim, and
reloads when files change. Use `--no-open` in scripts.

For a Vite app, let `soil dev` start Vite and keep Vite's HMR:

```sh
python tools/soil.py init --template vite my-app
cd my-app
npm install
python /path/to/soil-sdk/tools/soil.py dev . --frontend "npm run dev"
```

The Vite scaffold's `vite.config.js` injects the bridge in dev only. Production
still uses `vite build` followed by `soil build --web-dir dist`. You can use
`--frontend-url` if the frontend listens somewhere other than the `--port` URL.

## Commands

| Command | Purpose |
|---------|---------|
| `init DIR` | Scaffold an app (`--template vanilla` or `--template vite`). |
| `dev DIR` | Serve a live preview, or start a frontend dev server with `--frontend CMD`. |
| `build DIR` | Validate, pack and verify a package. Add `--sign KEY` to sign. |
| `run DIR\|FILE.sol` | Build if needed, then invoke the Soil runtime. |
| `verify FILE.sol` | Check format, digest and signature. |
| `keygen NAME` | Create `NAME.key.json` and `NAME.pub.pem`. |
| `doctor [DIR]` | Check Python, signing, runtime and app manifest. |

## Frontend build tools

`soil build` can stage a built web directory. This is the supported path for
Vite, React, Svelte, and any other tool that emits static files:

```sh
npm run build
python tools/soil.py build . --web-dir dist
python tools/soil.py build . --web-dir dist --entry index.html
```

The CLI copies `dist/` into the package's `web/` directory and rewrites the
packaged manifest entry. The source manifest still points at `web/index.html` for
normal development.

Preview a built output without repacking:

```sh
python tools/soil.py dev . --web-dir dist
```

## Development bridge differences

The `dev` server injects a browser-side implementation of `window.soil`:

* `soil.store` uses `localStorage` instead of the Soil data directory.
* `soil.dialog.openFile` returns the selected file name, not an absolute path.
* `soil.dialog.saveFile` asks for a name; it does not write a file.
* `soil.exec.run` is disabled in dev and rejects with an explanatory error.
* `soil.notify` uses the browser Notification API when available.
* `soil.window.setSize`/`minimize` are best-effort browser shims.

These differences are intentional. Run the packaged app in Soil before shipping
if it depends on native paths, `exec`, or real file writes.

## Signing and installing

```sh
python tools/soil.py keygen soil-sign
python tools/soil.py build . --sign soil-sign.key.json
python tools/soil.py verify App.sol --key soil-sign.pub.pem
soil trust soil-sign.pub.pem
soil run App.sol
```

The private key is written to `soil-sign.key.json`; never put it in the package
or commit it. The generated `.gitignore` excludes `*.key.json` and `*.sol`.

## Package layout

The scaffold uses:

```text
my-app/
  soil.json
  icon.png
  web/
    index.html
    app.js
    style.css
    soil.js
    soil.d.ts
```

Anything in the app directory is packed except VCS folders, `node_modules`,
`dist`, `__pycache__`, `.soil-build`, and root `.sol` files. With `--web-dir`,
only the staged web output is used instead of the source tree.
