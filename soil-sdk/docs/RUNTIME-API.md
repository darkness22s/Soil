# Soil runtime API (`window.soil`)

The Soil runtime is a WebKitGTK shell. App HTML is served through the custom
`soil://app/` scheme from the mounted package payload. A small bridge object is
injected into every frame before any app script runs.

All calls return promises. Errors reject with `Error(message)`.

```js
const info = await soil.app.info();
document.title = info.name;
```

## `soil.app`

| Member | Returns |
|--------|---------|
| `soil.app.info()` | `{id, name, version, kind, dataDir}` |

`dataDir` is `~/.local/share/soil/data/<id>` (created on launch; session-only in
the default Soil image because `/home` is tmpfs).

## `soil.window`

| Method | Notes |
|--------|-------|
| `soil.window.setTitle(string)` | Window title. |
| `soil.window.close()` | Closes the app window (same as user closing). |
| `soil.window.setSize(width, height)` | Clamps to sane desktop bounds. |
| `soil.window.minimize()` | Iconifies the window. |

## `soil.dialog` — requires `dialogs`

| Method | Returns |
|--------|---------|
| `soil.dialog.openFile({title, filters})` | `{path}` or `null` when cancelled |
| `soil.dialog.saveFile({title, defaultName})` | `{path}` or `null` |

`filters` is an array of `{name, patterns: ["*.txt"]}`.

## `soil.clipboard` — requires `clipboard`

| Method | Returns |
|--------|---------|
| `soil.clipboard.read()` | `{text}` |
| `soil.clipboard.write(text)` | `{}` |

## `soil.shell` — requires `openExternal`

| Method | Notes |
|--------|-------|
| `soil.shell.openExternal(url)` | Only `http:`, `https:` and `mailto:` are accepted; opens `soil-browser`. |

## `soil.notify` — requires `notifications`

```js
await soil.notify("Build finished", "3 warnings");
```

## `soil.store`

Session-scoped key/value store persisted at `<dataDir>/store.json`. Values are
strings. (Use `localStorage` for arbitrary JSON; both are session-scoped.)

| Method | Returns |
|--------|---------|
| `soil.store.get(key)` | `{value}` where value is string or `null` |
| `soil.store.set(key, value)` | `{}` |
| `soil.store.delete(key)` | `{}` |
| `soil.store.keys()` | `{keys: [...]}` |

## `soil.exec` — requires `exec`

```js
const {pid} = await soil.exec.run("bin/helper", ["--flag"]);
```

* `path` must be listed in `manifest.exec` and must stay inside the payload.
* No shell is used; arguments are passed as an array.
* Environment includes `SOIL_APP_ID`, `SOIL_VERSION`, `SOIL_APPDIR` (mount root),
  `SOIL_DATA` (data dir).
* Working directory is the app root.

## Behaviour and limits

* The bridge exists only inside the app webview; content cannot opt into it from
  the network because everything is served from `soil://app/`.
* Requests to unknown files under `soil://app/` return 404; path traversal
  (`..`) and symlink escapes are rejected.
* JavaScript dialogs (`alert`, `confirm`) are not implemented; use in-page UI.
* DevTools are disabled by default. Use `soil run --debug FILE.sol` (or
  `SOIL_DEBUG=1 soil run FILE.sol`) to enable the WebKit inspector and console
  output.
* A single window per app instance. Launching the same package twice starts a
  second instance; packages that need singleton behaviour should lock via
  `soil.store` or a file in `dataDir`.

## Example

```html
<!doctype html>
<meta charset="utf-8">
<title>Hello</title>
<h1>Hello Soil</h1>
<button id="b">Notify</button>
<script>
  b.onclick = async () => {
    const {id, name, version} = await soil.app.info();
    await soil.notify(name, `${id} ${version}`);
  };
</script>
```
