# Soil SDK 0.1.0

Build and ship **single-file, signed desktop apps** for Soil Linux. A `.sol`
package carries a JSON manifest and a squashfs payload with a tiny 128-byte
header; web apps use the Soil WebKitGTK 4.1 C shell (no Node or
Chromium in the shipped app).

```sh
python tools/soil.py init my-app
python tools/soil.py dev my-app
python tools/soil.py keygen my-key
python tools/soil.py build my-app --sign my-key.key.json
```

To package an existing native **Linux x86_64 ELF/AppImage** without a web UI:

```sh
python tools/soil.py native /path/to/App.AppImage --id org.example.app -o App.sol
# Or pass a prepared app directory with an executable AppRun:
python tools/soil.py native ./MyAppDir --entry AppRun -o MyApp.sol
```

Native `.sol` apps launch directly in the Soil Linux runtime. A `.sol` is
**not** a portable `.deb` or `.rpm`: host dynamic libraries, architecture,
and the guest distribution must still be compatible. See
[Native app packaging](docs/NATIVE-APPS.md).

For React + TypeScript:

```sh
python tools/soil.py init --template react-ts my-app
cd my-app
npm install
python /path/to/soil-sdk/tools/soil.py dev .
python /path/to/soil-sdk/tools/soil.py build . --sign my-key.key.json
```

The generated manifest records the frontend commands, so `soil dev .` starts
Vite and `soil build .` runs the TypeScript/Vite build before packing `dist`.
The `vite` template works the same way without React.

`soil.py` is the developer CLI: `init`, `dev` (localhost preview or external
frontend dev server + bridge shim + live reload), `build`, `run`, `verify`,
`keygen`, and `doctor`. Other frontend builders are supported with
`build . --frontend-build "CMD" --web-dir OUTPUT`; `--web-dir` alone still stages
an existing build. `soilmk.py` remains the low-level packer/signer. On Windows,
`tools\soil.cmd` forwards to the same Python CLI. See
[`docs/CLI.md`](docs/CLI.md).

On Soil:

```sh
soil install MyApp.sol
soil launch org.example.myapp
soil remove org.example.myapp
# or run without installing:
soil run MyApp.sol
```

## Layout

| Path | Purpose |
|------|---------|
| `docs/` | Format, manifest, packaging, runtime API, security, install/run, testing |
| `schema/soil.schema.json` | JSON Schema for `soil.json` |
| `tools/soil.py` | Developer CLI: scaffold, live preview, build, run |
| `tools/soilmk.py` | Low-level pack / sign / verify / keygen |
| `tools/mksquashfs.py` | Pure-Python gzip squashfs writer (no host tools needed) |
| `templates/` | Vanilla, Vite, and React+TypeScript starters used by `soil init` |
| `runtime/` | C sources for the CLI (`soil.c`) and WebKitGTK runtime (`soil-app.c`) |
| `runtime/test_core.c` | Host unit tests (SHA-256, JSON, package header/signature) |
| `runtime/test-keys/` | Signed `CoreTest.sol` fixture + its public key |
| `runtime/build_guest.sh` | Reference in-VM build recipe (Tiny Core dev farm) |
| `runtime/soil-app` | Wrapper script used by `soil run` |
| `samples/` | Hello (bridge demo), Frontend (React UI), DemoInstaller, signed `.sol` files |
| `package/` | MIME type, handler desktop entry, default-associations file |
| `skill/soil-app-builder/` | AI-agent skill: author and ship a `.sol` app |
| `skill/soil-integrator/` | AI-agent skill: deploy, trust, install, bake into images |

## Requirements

* **Packaging host** — Python 3.8+. No third-party packages unless you sign
  (then `cryptography`). Works on Windows, macOS and Linux. `soil dev` uses only
  the Python standard library. The optional Vite and React+TypeScript templates
  also need Node/npm on the dev host; Node is not shipped in the app.
* **Runtime** — Linux with GTK3 + WebKitGTK 4.1, `openssl` CLI, `sudo`, and
  loop devices. The Soil image ships all of it.

## Samples

* `samples/Hello.sol` — signed webapp that exercises store, clipboard,
  notifications, dialogs and title sync.
* `samples/Frontend.sol` — signed React dashboard UI (bundled vendor files).
* `samples/DemoInstaller.sol` — signed installer-kind package with a shell hook.

The sample signing key's **public** half is `samples/keys/demo.pub.pem`
(fingerprint `d75cbf013eeaeacb`); the image trusts it as `soil-demo.pem`. The
private demo key is not part of the distributed bundle — generate your own with
`soilmk keygen`.

## Documentation

Start with `docs/CLI.md` for the developer workflow, `docs/SOL-FORMAT.md` for the
container, `docs/PACKAGING.md` for the build flow and `docs/RUNTIME-API.md` for
`window.soil`.

## Known limitations

* The stock Soil kernel has no `binfmt_misc`; `.sol` files are launched via
  `soil run`, the MIME handler, menu entries or desktop icons.
* Installs and app data live under `/home` (tmpfs), so they last for the session.
* One window per app instance; JavaScript `alert`/`confirm` are not implemented.
* Web helpers must be declared in `manifest.exec` (kind `webapp+native` or
  `installer`); there is no shell execution.
* `soil dev` emulates the bridge in the browser; file paths, `exec` and real
  native writes must be tested in Soil.
* `soil dev --web-dir` previews built frontend output instead of starting the
  configured dev command.
