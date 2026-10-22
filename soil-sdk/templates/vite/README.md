# __NAME__

A Vite app packaged by the Soil SDK. The dev server injects the `window.soil`
bridge; the production build is packed with `soil build --web-dir dist`.

## Develop

```sh
npm install
python /path/to/soil-sdk/tools/soil.py dev . --frontend "npm run dev"
```

`soil dev` starts Vite, waits for it, and opens the app. Vite's HMR stays
available. The bridge is injected only in dev (`vite.config.js`); it is not
included in the production output.

From the SDK root you can run the same command with a path:

```sh
python tools/soil.py dev path/to/this/app --frontend "npm run dev"
```

## Build

```sh
npm run build
python /path/to/soil-sdk/tools/soil.py build . --web-dir dist
```

The package is written as `__OUT__` next to this file. To sign it:

```sh
python /path/to/soil-sdk/tools/soil.py keygen soil-sign
python /path/to/soil-sdk/tools/soil.py build . --web-dir dist --sign soil-sign.key.json
```

The generated SDK CLI keeps a copy of the dev bridge in `dev/soil-bridge.js`.
That file is only served by Vite in dev; `--web-dir dist` packages only the
built output.

## Layout

```text
soil.json          Soil manifest
index.html         Vite entry
src/main.js        app code
src/soil.js        window.soil helper
src/soil.d.ts      bridge types
vite.config.js     Vite + dev bridge plugin
dev/soil-bridge.js generated bridge shim (dev only)
```
