# Cua Driver and Soil VM computer use

## Current implementation

Every **Windows-hosted Soil VM** now has a VM-scoped computer-use transport exposed by the Soil app itself:
`GET /api/vms/{id}/computer/screenshot`, `POST /api/vms/{id}/computer/action`,
`Soil.exe vm computer ...`, and an stdio MCP server `Soil.exe vm computer mcp`.
The bridge is available before installing anything in the Linux guest, and a
stopped VM has no computer-use process. The MCP tools are `soil_list_vms`,
`soil_vm_screenshot`, and `soil_vm_action`; actions return a fresh screenshot
for visual verification. Access to the computer endpoints requires the
per-install token stored under Soil's `keys/` directory.

The VM-scoped bridge uses Soil's existing host framebuffer and PS/2 input
transport. **It is not the upstream `cua-driver` binary or SDK.** The MCP
protocol and screenshot/action/verification loop are inspired by the
open-source Cua Driver agent interface, but the runtime is currently Soil's
own VM input transport. We intentionally do not claim to expose Cua Driver's
accessibility tree, per-window background input, or pixel-perfect absolute
pointer targeting: the VM engine currently has only relative PS/2 motion.

## Why the official Cua Driver is not in Tiny Core

Cua Driver, maintained at https://github.com/trycua/cua in
`libs/cua-driver/`, is MIT licensed. It is designed to run **inside a
supported interactive desktop session**. Linux support assumes a compatible
glibc-based distribution, X11/XWayland and AT-SPI 2 for accessibility.
Soil's current default Windows guest is a compact Tiny Core Linux image
(`vmlinuz64`, `core2.gz`, `soil.squashfs`), not a complete glibc
desktop. Simply copying a Cua Driver executable into `core2.gz` would be a
broken/nonlaunching installer and is deliberately avoided.

To fully meet the requested **preloaded upstream Cua Driver in every VM**
feature, the default guest image must first be replaced or extended with a
supported glibc/X11/AT-SPI runtime. Then pin a verified Cua Driver release,
ship its actual MIT license text and dependency notices, provision an
unprivileged guest daemon on *explicit VM start*, gate its MCP/IPC channel to
the selected VM, and test screenshot, accessibility snapshot, click, text,
and window targeting in the **actual booted guest** before shipping. Its
lifetime should follow VM start/stop and preserve isolation. Unsupported
guest variants must return a capability error, not pretend Cua Driver is
running.

## Agent instructions today

Use `Soil.exe vm computer mcp` to connect a local MCP-capable agent, or
`Soil.exe vm computer screenshot <id> file.jpg` and
`Soil.exe vm computer action <id> '{...}'`. Always inspect a screenshot
before acting and verify the returned post-action screenshot. Keyboard/text
are ASCII PS/2; mouse coordinates can drift with guest acceleration.
Scroll, drag, accessibility trees and Unicode typing are not yet supported.
Do not claim an imported AppImage opened without an observed frame.

## Upstream references

- Cua Driver: https://github.com/trycua/cua/tree/main/libs/cua-driver
- Upstream Linux platform design: https://github.com/trycua/cua/blob/main/docs/content/docs/cua-driver/concepts/how-cua-driver-works.mdx
- Install prerequisites: https://github.com/trycua/cua/blob/main/docs/content/docs/cua-driver/quickstart.mdx
- Cua project license: https://github.com/trycua/cua/blob/main/LICENSE

No Cua Driver binaries, model weights or source code have been vendored in the
current Soil bridge, so no Cua redistributable license is being attached to
the installer.
