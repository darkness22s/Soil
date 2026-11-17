# Cua Driver and Soil VM computer use

## Current implementation

The Windows Soil installer now **embeds an authentic upstream Cua Driver
0.49.5 x86_64 ELF** and its MIT notice into the guest `core2.gz` at
`/bin/cua-driver`. The release archive is checksum pinned:
`e01fe9dd44e6a7d2d8ac128737fb0204c6deb57929da6cc2871c049eda96f7dc`
(https://github.com/trycua/cua/releases/tag/cua-driver-rs-v0.49.5).
The actual upstream binary is downloaded **at build time only**, not on
user devices. The license text is included at
`/usr/share/doc/cua-driver/LICENSE.md` in the VM and bundled
in the Windows installation. See `tools/soil-ci/fetch-cua-driver.sh`,
`inject-native-guest.py`, `app/build.ps1` and both Windows workflows.

Every Windows-hosted Soil VM also has host-side screenshot and PS/2
action endpoints and a stdio MCP server. The agent tools are
`soil_list_vms`, `soil_vm_screenshot`, `soil_vm_action`, and
`soil_vm_cua_call`. The last tool invokes the **genuine upstream
guest binary**, without exposing SSH credentials. The guest daemon is
started on first use, with upstream telemetry explicitly disabled.
No guest Cua daemon runs while its VM is stopped.

## Remaining runtime limitation: guest graphical desktop

A real QEMU boot proved the upstream binary dynamically links and runs.
Its `serve` daemon starts, but native `get_desktop_state` failed when
the default guest did not start an X11 server. `/dev/fb0` was not an
operational kernel framebuffer in that test, so the default guest
`Xfbdev` server also failed. This is **not** an AppImage import failure:
the VM must provide a real X11 desktop before Cua can capture, click
or expose accessibility/window state.

The host-side computer tools remain usable for machine frames even if
the native Cua interface is unavailable. The app launch path now waits
for `DISPLAY=:0 xset q` instead of accepting SSH as GUI readiness and
returns a specific error on failure. The dedicated Cua CI probe is
non-blocking and intentionally checks actual `get_desktop_state`,
not just `--version`. Both framebuffer Xfbdev (no usable `/dev/fb0`)
and virtual Xvfb (segmentation fault in the shipped guest) failed in
the tested image. Do **not** turn on the crashing virtual-display
fallback in production or claim working graphical computer use from
this result.

Cua Driver requires a working X11/XWayland session; AT-SPI window
accessibility additionally requires an AT-SPI D-Bus bus. Tiny Core itself
uses glibc and our bundled guest resolves the driver's direct shared
libraries. The engineering next step is ensuring the VM's graphical
session starts and the Soil viewer shows that same session; native
desktop automation is not declared fully verified until it does.

## Agent instructions today

Use `Soil.exe vm computer mcp` to connect a local MCP-capable agent, or
`Soil.exe vm computer screenshot <id> file.jpg` and
`Soil.exe vm computer action <id> '{...}'`. Always inspect a screenshot
before acting and verify the returned post-action screenshot. Keyboard/text
are ASCII PS/2; mouse coordinates can drift with guest acceleration.
The host bridge lacks scroll, drag, accessibility trees and Unicode typing; the upstream Cua Driver tool surface can provide those capabilities only when its guest X11/AT-SPI session is operational.
Do not claim an imported AppImage opened without an observed frame.

## Upstream references

- Cua Driver: https://github.com/trycua/cua/tree/main/libs/cua-driver
- Upstream Linux platform design: https://github.com/trycua/cua/blob/main/docs/content/docs/cua-driver/concepts/how-cua-driver-works.mdx
- Install prerequisites: https://github.com/trycua/cua/blob/main/docs/content/docs/cua-driver/quickstart.mdx
- Cua project license: https://github.com/trycua/cua/blob/main/LICENSE

The Windows build now bundles the authentic pinned Cua Driver ELF and its MIT notice; it does not vendor the unrelated Cua Spaces or Cua Driver Bench components.
