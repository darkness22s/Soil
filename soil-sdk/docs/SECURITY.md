# Security model

## Trust chain

* **Integrity**: every `.sol` carries `SHA256(manifest || payload)`. The runtime
  checks it before mounting or installing. Corruption/truncation is always
  rejected.
* **Authenticity (optional)**: Ed25519 signature over the digest. Signed packages
  are accepted silently only when the signing key is in a trust store:
  * system: `/usr/local/share/soil/trust/*.pem` (baked into the image by an admin)
  * user: `~/.config/soil/trust/*.pem` (`soil trust FILE.pem`)

Unsigned or unknown-key packages produce a warning:

```
soil: warning: untrusted package (unsigned)
```

and run in normal mode. Set `SOIL_STRICT=1` (or `soil run --strict`) to refuse
them. Set `SOIL_ALLOW_UNTRUSTED=1` to silence the warning explicitly.

## Runtime isolation

* App content runs in the WebKitGTK web process with the image's CPU-only WebKit
  configuration. The system browser's protections apply (site isolation is not
  used at 200 MiB RAM; keep untrusted content out of packages).
* The native bridge is capability-gated by `manifest.permissions`. A call to an
  ungranted capability rejects.
* `soil.exec` can only launch executables listed in `manifest.exec`, resolved
  inside the read-only payload mount. No shell is involved.
* `soil://app/` serves files from the payload only. `..` traversal and symlink
  escapes are rejected; symlinks are forbidden in payloads entirely (v1).
* Packages run as the desktop user. `install.root` hooks run through
  `sudo -n`; treat root-installer packages exactly like system packages.

## What a `.sol` is NOT

* It is not a sandbox for malicious native code. Kind 2/3 packages bundle
  executables and hooks that run with user (or root, if requested) privileges.
* It is not DRM. Anyone with the signing key can produce packages; distrust
  is enforced only through the trust store.

## Key handling

* `soilmk keygen` writes `NAME.key.json` (seed + public key, base64). Keep the
  seed secret; never ship it in an app payload or the distro image.
* `NAME.pub.pem` is the distribution artifact. Publish it alongside your
  packages.
* The image ships one trusted key: the **Soil Samples** key, used only for the
  bundled demo packages. Do not reuse it for production packages.

## Reporting

This is a hobby distribution; there is no CVE process. Report issues to the
distro maintainer.
