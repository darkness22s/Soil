# Testing the Soil SDK

## Host-side checks (no Soil needed)

```sh
python tools/soil.py init /tmp/demo
python tools/soil.py build /tmp/demo
python tools/soil.py verify /tmp/demo/Demo.sol
```

`soil build` checks the manifest, packs, and verifies the digest. `verify`
checks the header, 512-byte payload alignment, size bounds, and digest. With
`--key published.pem` it also verifies the Ed25519 signature against a public
key.

For the full developer loop:

```sh
python tools/soil.py dev /tmp/demo --no-open
# in another shell:
curl -fsS http://127.0.0.1:5173/ | grep __soil/bridge.js
```

C unit tests for the runtime primitives:

```sh
gcc -O2 -o test_core runtime/test_core.c runtime/sol.c runtime/json.c \
    runtime/sha256.c
mkdir -p ~/.config/soil/trust
cp runtime/test-keys/core-test.pub.pem ~/.config/soil/trust/core-test.pem
./test_core runtime/test-keys/CoreTest.sol
```

The test binary covers SHA-256 vectors, JSON escape/round-trip/rejection, header
validation, manifest parsing and digest/signature verification. It exits non-zero
on the first failure.

## Native Linux package checks

```sh
python3 -m unittest discover -s tests -p test_native_packages.py -v
gcc -O2 -o /tmp/soil-cli runtime/soil.c runtime/sol.c runtime/json.c runtime/sha256.c
python3 tools/soil.py native /bin/true --id org.soil.true -o /tmp/True.sol
/tmp/soil-cli info /tmp/True.sol
```

These confirm native package creation and C header parsing. Mounting and
launching a graphical Linux application inside the bundled guest still needs
a real guest smoke test with the rebuilt Linux runtime.

## In-image checks (Soil VM)

The shipped image contains `Hello.sol`, signed by the demo key whose public key
is baked at `/usr/local/share/soil/trust/soil-demo.pem`.

```sh
soil verify /usr/local/share/soil/samples/Hello.sol
soil run /usr/local/share/soil/samples/Hello.sol
```

Expected: `digest: OK`, `signature: valid (trusted key: soil-demo.pem)`, then a
1024x768 window titled `Hello Soil — launch #N`. The counter increments on every
launch (`store.json` in `~/.local/share/soil/data/org.soil.hello/`).

Install / remove:

```sh
soil install /usr/local/share/soil/samples/Hello.sol
soil list
soil launch org.soil.hello
soil remove org.soil.hello
```

Tamper detection:

```sh
cp Hello.sol /tmp/T.sol
printf 'x' >> /tmp/T.sol
soil verify /tmp/T.sol        # digest: MISMATCH, exit code 2
```

Untrusted signature: trust only the shipped demo key, sign with another key and
run `soil verify` (reports untrusted). `SOIL_STRICT=1 soil run App.sol` refuses;
without strict mode the app runs after a warning.

## MIME / desktop integration

`/usr/local/share/mime/mime.cache` and
`/usr/local/share/applications/mimeinfo.cache` are pre-built with
`application/x-soil-package=soil-package.desktop`, and
`/usr/local/share/applications/mimeapps.list` sets the default handler to
`soil run %f`. The kernel has no `binfmt_misc`; launching is always through
`soil run`, the MIME handler, the Fluxbox menu, or installed desktop icons.

## Release checklist

1. `python -m py_compile tools/soilmk.py tools/mksquashfs.py`
2. Rebuild `runtime` in a WebKitGTK 4.1 dev environment (see
   `runtime/build_guest.sh`) and smoke-test both webapp and installer samples.
3. Verify each sample with `soilmk verify` and `soil verify`.
4. Rebuild the image, boot it at its target RAM, and run the in-image checks.
5. Confirm `soil docs` lists all seven guides plus the JSON schema.
