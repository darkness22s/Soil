# __NAME__

A React + TypeScript app packaged by the Soil SDK. The dev server injects the
`window.soil` bridge; production output is built with Vite and packed into
`__OUT__`.

## Develop

```sh
npm install
python /path/to/soil-sdk/tools/soil.py dev .
```

`soil dev` reads `dev.command` from `soil.json`, starts Vite, waits for it, and
opens the app. Vite's HMR stays available. The bridge is injected only in dev
(`vite.config.ts`); it is not included in the production output.

From the SDK root you can run the same command with a path:

```sh
python tools/soil.py dev path/to/this/app
```

## Build

```sh
python /path/to/soil-sdk/tools/soil.py build .
```

`soil build` runs `build.command` (`npm run build`), stages `build.webDir`
(`dist`), and writes `__OUT__` next to this file. To sign it:

```sh
python /path/to/soil-sdk/tools/soil.py keygen soil-sign
python /path/to/soil-sdk/tools/soil.py build . --sign soil-sign.key.json
```

Skip the frontend build when `dist/` is already current:

```sh
python /path/to/soil-sdk/tools/soil.py build . --no-build
```

Override the configured build command or output directory:

```sh
python /path/to/soil-sdk/tools/soil.py build . --frontend-build "npm run build" --web-dir dist
```

## Layout

```text
soil.json             Soil manifest + dev/build commands
index.html            Vite entry
src/main.tsx          React entry
src/App.tsx           app UI
src/soil.ts           typed window.soil helper
src/soil-env.d.ts     bridge types
vite.config.ts        Vite + React + dev bridge plugin
dev/soil-bridge.js    generated bridge shim (dev only)
```
