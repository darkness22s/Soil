# Morning Report — 2026-10-19

## Update (after user feedback)
Terminal no longer auto-opens on boot (removed from `XSESSION`). The browser
you saw was a leftover verification window, not an autostart — a fresh boot
now shows only the desktop (pcmanfm + fluxbox). At 280 MB: 72 MB used /
313 MB available with nothing open; both Terminal and Browser launch only
on demand (desktop icon / fluxbox menu).

## TL;DR
Chrome is gone from the Soil image and replaced by `soil-browser`
(GTK3 + WebKitGTK 4.1, custom build). The blocker that consumed most of the
night — WebKitGTK hard-aborting on EGL — is fixed with a purpose-built EGL
shim. Verified end-to-end at **280 MB RAM**: boot to SSH in ~8 s, browser
opens and renders `http://example.com` (window title "Example Domain"),
278 MB available after launch, zero missing libs, no OOM. VM left running
at 280 MB.

## What was done

### 1. Chrome removed, browser replaced (image: 313.9 → 150.9 MiB)
- `/opt/google` + `google-chrome` wrappers no longer baked; BOOTLOCAL
  bind-mount removed.
- Desktop icon `Browser.desktop` → `Exec=soil-browser`.
- Fluxbox menu entry `Browser` → `soil-browser`.
- `/usr/local/share/applications/soil-browser.desktop` (google-chrome
  desktop entry dropped). Only one browser entry left in the image.

### 2. EGL fix (the real blocker)
Failure chain found:
- WebKitGTK aborts (`eglQueryString() not found`) when libEGL is just the
  glvnd stub (original image state).
- The repo's `libEGL.tcz` is stale: its libEGL references
  `libgallium-26.2.4.so`, but the matching mesa 26.2.4 is nowhere; with the
  shippped mesa 25.2.5 gallium it segfaults (DRI3 path, no /dev/dri).
- Making it worse: WebKitGTK *requires* a non-null EGL display — a pure
  "unavailable" stub still gets `Could not create default EGL display`.
  Aborting...`.

Solution (new): **`libEGL-stub.so.1.0.0`** — a ~17 KB stub compiled in-guest
(gcc from the dev tcz farm) exporting all 56 EGL 1.4/1.5 entry points:
- `eglGetDisplay`/`eglGetPlatformDisplay*` return a dummy display,
- `eglInitialize` reports EGL 1.4, `eglQueryString` advertises **no
  extensions** (so WebKit skips GBM/device probing),
- all config/context creation fails gracefully → WebKit falls back to CPU
  rendering (no GPU in the VM anyway).
Baked as the `libEGL.so.1.0.0` file; symlink chain `libEGL.so.1 → .so.1.0.0`
preserved (ld.so.cache entry unchanged).

### 3. Missing `org.gnome.system.proxy` schema
Real http(s) page loads crashed the network process: a stale 2.3 KB
`gschemas.compiled` (from `bootdelta.tgz`) was overriding the baked
schemas. Fixed by:
- recompiling schemas in-guest with `glib-compile-schemas` (39,228 B,
  includes all 5 `org.gnome.system.proxy*` schemas),
- replacing the compiled blob in `assets/gschemas/`,
- dropping the stale entry from `bootdelta.tgz`.

### 4. Boot / RAM
- Kernel cmdline: `console=ttyS0 quiet loglevel=3 acpi=off noapic nolapic
  idle=poll nozswap` (kept as-is; `mitigations=off`/zstd-initrd judged
  not worth the risk for <0.5 s).
- Boot to SSH: **~8.0 s** at 280 MB (`/proc/uptime` 6.7 s).
- At 280 MB with browser + WebKit processes on example.com:
  used 107 MB, available **278 MB**; no OOM, no dmesg faults.
- 480 MB config still boots (verified earlier in the night).

## Verification matrix (all on the final baked image, no shims)
| Check | Result |
|---|---|
| Boot 280 MB → SSH | 8.0 s |
| `ldd soil-browser-bin` missing libs | 0 |
| Browser launch → windows | "Soil Browser" + "Example Domain" |
| WebKit processes alive | WebKitWebProcess + WebKitNetworkProcess |
| Runtime log | clean (no GLib/EGL/WebKit errors) |
| Desktop icon / fluxbox menu | Browser → soil-browser |
| `/opt/google`, google-chrome | absent |
| Image size | 150.9 MiB (was 313.9) |

## Artifacts
- Stub source: `drv/eglstub2.c`; built binary: `remaster/assets/libEGL-stub.so.1.0.0`
- Schema asset: `remaster/assets/gschemas/gschemas.compiled` (39,228 B)
- remaster.py: EGL stub bake + proxy deltas; `bootdelta.tgz` cleaned
- Test scripts: `drv/stub5.sh`, `drv/e2e.sh`, `drv/f.sh`, `drv/g.sh`, `drv/v.sh`

## Caveats / next steps
- Software rendering only — that's by design (VM has no GPU); if a GPU is
  ever added, swap the stub for a version-matching mesa libEGL.
- MiniBrowser remains built-in; fine in this image.
- Engine/MCP: no `google-chrome` references found in `engine/`, but any
  external MCP flows that call `launch_app("google-chrome")` should now
  call `soil-browser`.
- Optional still-open speed items: `mitigations=off`, zstd initrd, xkb
  precompile, hook trims (est. <0.5 s combined; current 8 s judged fine).
