# Soil Package Format (`.sol`) v1

A `.sol` file is a single-file, self-contained, signed application package for the
Soil Linux distribution. It behaves like an executable: launch it with `soil run`
(or, on systems that provide binfmt_misc, execute it directly when the execute bit
is set and the `S0L1` handler is registered).

Internally it is:

```
+-------------------------------+
| 128-byte fixed header         |
+-------------------------------+
| manifest (UTF-8 JSON)         |
| padded with spaces to 512 B   |
+-------------------------------+
| payload (squashfs, gzip)      |
+-------------------------------+
```

The payload starts at a 512-byte aligned offset (`payload_offset`). The padding is
part of the manifest region, so `header_len = payload_offset` and the digest covers
the padded bytes.

The payload is a squashfs image (version 4.0, gzip compression). Soil mounts it
read-only at launch — nothing is extracted to RAM or disk, so package size does not
consume memory beyond page cache.

## Magic

The first four bytes are `S0L1` (ASCII, 0x53 0x30 0x4C 0x31). Hosts with binfmt_misc
can register this prefix (`:soil:M::S0L1::/usr/local/bin/soil:P`); the stock Soil
kernel does not include binfmt_misc, so Soil registers the `application/x-soil-package`
MIME type instead and `.sol` files are launched with `soil run` from the file
manager, menu or shell.

## Header layout (little-endian)

| Offset | Size | Field | Notes |
|--------|------|-------|-------|
| 0      | 4    | magic | `S0L1` |
| 4      | 2    | format_version | `1` |
| 6      | 2    | kind | 1 = webapp, 2 = installer, 3 = webapp+native, 4 = native |
| 8      | 4    | header_len | equals `payload_offset`; `128 + manifest_len` (manifest_len includes padding) |
| 12     | 4    | manifest_len | bytes of manifest JSON plus space padding |
| 16     | 8    | payload_offset | byte offset of squashfs payload; multiple of 512 |
| 24     | 8    | payload_size | byte length of squashfs payload |
| 32     | 32   | digest | SHA-256 over `manifest_bytes || payload_bytes` |
| 64     | 64   | signature | Ed25519 signature over the 32-byte digest; all-zero = unsigned |
| 128    | —    | manifest | UTF-8 JSON |

All integers are unsigned little-endian. Readers must reject files where
`payload_offset` is not 512-byte aligned or where
`payload_offset + payload_size` differs from the file size (no trailing garbage).

## Digest and signature

* `digest = SHA256(manifest_bytes || payload_bytes)` where `manifest_bytes` includes
  the space padding and `payload_bytes` is the squashfs image exactly as stored
  after `payload_offset`.
* If the signature block is non-zero, it is an Ed25519 signature over the 32-byte
  `digest` (Ed25519 message = digest bytes, not prehashed curve arithmetic).
* Signing keys are distributed as PEM SubjectPublicKeyInfo (Ed25519) files in the
  trust store:
  * system: `/usr/local/share/soil/trust/*.pem`
  * user:   `~/.config/soil/trust/*.pem`
* Verification uses OpenSSL (`openssl pkeyutl -verify -rawin`). Packages that are
  unsigned or signed by an unknown key are reported as **untrusted**; Soil runs
  them with a warning unless strict mode is enabled (`SOIL_STRICT=1` or
  `soil run --strict`).

## Payload layout

The squashfs root contains the app files exactly as authored, plus a copy of the
manifest at `/manifest.json` (unpadded; the header copy is authoritative).

* `entry` points at the web HTML entry (kinds 1 and 3), or an executable\n  Linux entry (kind 4), relative to payload root
  (e.g. `web/index.html`).
* `icon` points at a PNG, relative to payload root.
* Symlinks and hardlinks are rejected by `soilmk pack` in v1 (keeps the payload
  sandbox and the mount semantics simple).
* Web helpers (kind 3) use `exec`; installer hooks (kind 2) use\n  `install.command`; kind 4 launches its `entry` directly.

## Execution and lifecycle

`soil run FILE.sol` (or opening the file from the desktop / file manager):

1. Parses the header and manifest, checks size bounds and digest.
2. Verifies the signature against the trust store.
3. Mounts the payload read-only with a loop device (`LO_FLAGS_AUTOCLEAR`) under a
   private directory in `/tmp` (requires the `sudo -n soil _mount` helper; Soil's
   `tc` user has passwordless sudo).
4. Launches `soil-app` for a web kind, or executes the native `entry`\n   directly for kind 4.
5. When the window closes, unmounts (lazy) and releases the loop device.

`soil install FILE.sol`:

1. Verifies the package.
2. Copies it to `~/.local/share/soil/pkg/<id>-<version>.sol`.
3. Extracts the icon to `~/.local/share/soil/icons/<id>.png`.
4. Writes `~/.local/share/applications/soil-<id>.desktop` and a launcher on the
   desktop (`~/Desktop/<Name>.desktop`).
5. For installer-kind packages, extracts the payload to
   `~/.local/share/soil/opt/<id>/` and runs `install.command` there.

Installs live in `/home`, which Soil mounts as tmpfs per session (RAM plan). They
persist for the session only. To run packages without installing, use them
directly from attached storage (e.g. `/mnt/sda1/App.sol`) or bake them into a
custom image.

## Limits

* `manifest_len` ≤ 1 MiB.
* payload ≤ 4 GiB (u32-aligned squashfs constraints; practical limit is RAM/disk).
* Package id: `^[a-z0-9][a-z0-9._-]{1,63}$`.
* Version: semver `MAJOR.MINOR.PATCH`.
