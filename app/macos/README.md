# Soil for macOS (Apple Silicon preview)

This is an early macOS launcher for the existing x86_64 Soil guest. It uses QEMU's
software TCG emulation because the current guest is x86_64 and Apple Silicon's
Hypervisor.framework cannot execute x86_64 guests. Expect substantially lower
performance than native virtualization.

This is a limited preview, not a port of the Windows desktop application. The
QEMU guest runs headlessly and its framebuffer and keyboard/mouse input are
routed through an in-app VNC viewer over a private Unix-domain socket. Machine
management, the Files view, app API, snapshots, SSH forwarding, and the custom VM
devices remain Windows-only.

## Requirements

- Apple Silicon Mac running macOS 13 or later
- Xcode Command Line Tools with Swift 5.9 or later
- QEMU installed with Homebrew (`brew install qemu`)
- The three existing x86_64 guest images: `vmlinuz64`, `core2.gz`, and
  `soil.squashfs`

Put the images in `~/Library/Application Support/Soil/images/`. The image files
are not copied into this repository or the app bundle.

## Build and launch

From the repository root on the Mac:

```sh
./app/macos/build.sh
open ./app/macos/dist/Soil.app
```

The app's Start button launches QEMU without a host display or terminal; the guest
screen and keyboard/mouse input appear in the Soil window over a VNC connection
bound only to loopback. Stop terminates QEMU. The launcher searches the standard
Apple Silicon and Intel Homebrew binary paths for `qemu-system-x86_64`.

## Current limitations

The guest boot configuration has not yet been validated under QEMU. The existing
guests and device models were developed for the custom Windows Hypervisor
Platform engine, so QEMU device compatibility and guest startup may require
additional work. This preview does not claim parity with Soil for Windows.
