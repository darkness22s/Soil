# __NAME__

A desktop app built for Soil with the Soil SDK.

## Develop

From the SDK root:

```sh
python tools/soil.py dev path/to/this/app
```

Or from this directory, using the SDK path:

```sh
python /path/to/soil-sdk/tools/soil.py dev .
```

This serves the app on localhost, injects the `window.soil` bridge, and reloads
when files change.

## Build

```sh
python /path/to/soil-sdk/tools/soil.py build .
```

The package is written as `__OUT__` next to this file. To sign it, create a key
and pass it to the build:

```sh
python /path/to/soil-sdk/tools/soil.py keygen soil-sign
python /path/to/soil-sdk/tools/soil.py build . --sign soil-sign.key.json
```

If you use Vite or another frontend builder, stage its output instead of packing
the source directory:

```sh
python /path/to/soil-sdk/tools/soil.py build . --web-dir dist
```

## Run in Soil

```sh
soil trust soil-sign.pub.pem
soil run ./__OUT__
```

`soil store` values are session-scoped in the default Soil image because
`/home` is a tmpfs.
