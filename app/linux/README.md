# Soil for Linux (x86_64 preview)

Soil now has a Linux-native **host launcher**, separate from its Windows
WinForms/WebView2 + WHPX engine. It starts the actual Soil `app.html` UI in
your browser, manages manually created VMs under `~/.soil`, runs QEMU with KVM
when available (otherwise multi-threaded TCG), and embeds the guest console via
noVNC. A native Windows executable or Wine is **not** involved.

## Linux prerequisites

On Ubuntu or Debian:

```sh
sudo apt update
sudo apt install python3 qemu-system-x86 qemu-utils novnc websockify
```

Run from the source checkout:

```sh
python3 app/linux/soil.py --check
python3 app/linux/soil.py
```

Or build and install the portable tarball:

```sh
bash app/linux/build.sh
tar -xzf app/linux/dist/Soil-Linux-x86_64.tar.gz
./Soil-Linux/install.sh
~/.local/bin/soil
```

The launcher opens your default browser at a loopback-only, random-port URL.
To run without opening a browser, pass `--no-browser`. Use Ctrl+C to stop the
host and its running VMs. For faster VMs, enable hardware virtualization and
grant your user access to `/dev/kvm` (usually membership in the `kvm` group;
log out/in after changing groups).

## Boot a Linux installer ISO inside a Soil VM

Use **Install Linux from ISO** on Soil's Linux `app.html` interface:

1. Choose an x86_64 Linux installer `.iso` file (e.g. Debian, Ubuntu, Fedora).
2. Name the VM. Soil creates a **4 GiB RAM, 2 vCPU, 80 GiB virtual-disk**
   QEMU machine. It creates no VM until this action is chosen.
3. The ISO is **uploaded to the Soil home and attached read-only as a
   virtual CD-ROM**; the QEMU VM boots from it. Complete the distribution's
   installer using Soil's existing noVNC screen, mouse and keyboard.
4. Install to the VM's virtual disk (normally **`/dev/vda`**). The disk is
   a separate sparse QCOW2 image at
   `~/.soil/machines/<vm-id>/data/linux-disk.qcow2`.
5. After the installer reboots, QEMU prefers the virtual hard disk. Subsequent
   Soil **Start** calls boot the disk rather than reinstalling Linux. Stop
   the VM to **Eject installer ISO**, **Replace Linux ISO**, or choose
   **Boot ISO on next Start** if another installer boot is required.

You can always stop and start these VMs, and delete them through the normal VM
controls. Disk changes survive restart. Linux installer VMs **do not depend on
Soil's private vmlinuz64/core2.gz/soil.squashfs guest images**, and the
installer executes **inside QEMU**, not as a native host application.
The host uses QEMU's x86 BIOS/El Torito path for bootable x86_64 ISO media;
UEFI-only installers, ARM images, raw `.img` media and automatic unattended
OS installation are not yet supported. Without KVM, QEMU TCG works but may be
too slow for a graphical desktop installer. Use an ISO supplied by a trusted
Linux distribution.

The installer upload endpoint streams raw bytes without buffering the ISO in
memory, validates the ISO9660 descriptor, limits media to 12 GiB, and replaces
old media only after successful validation.

```sh
# Obtain <PORT> from Soil's startup URL.
# Create a stopped VM. Save the returned ID.
curl -H 'Content-Type: application/json' -d \
  '{"name":"Ubuntu","size":"standard","boot":"linux-iso"}' \
  http://127.0.0.1:<PORT>/api/vms

curl -X POST -H 'Content-Type: application/octet-stream' \
  -H 'X-Soil-Filename: ubuntu.iso' --data-binary @ubuntu.iso \
  http://127.0.0.1:<PORT>/api/vms/<ID>/installer
curl -X POST -H 'Content-Type: application/json' -d '{}' \
  http://127.0.0.1:<PORT>/api/vms/<ID>/start
# GET /api/vms/<ID>/console, POST /api/vms/<ID>/stop
# POST /api/vms/<ID>/installer/boot, DELETE /api/vms/<ID>/installer
```

**Host limitation:** This ISO boot feature currently uses the separate
**Linux QEMU host** under `app/linux`. The Windows WHPX engine does not
include PC BIOS/UEFI firmware or generic ISO boot support, so it cannot boot
these installer ISOs yet.

## Guest images (required for Soil Desktop VMs, not for installer VMs or the UI)

Place **real, complete** `vmlinuz64`, `core2.gz`, and `soil.squashfs` files
in `~/.soil/images/`. Git LFS pointer files do not count. The Linux tarball
intentionally does not redistribute private guest images.

Use **New VM**, choose a resource size and click **Done** to create
a Soil Desktop VM; press **Start** to boot it. A separate per-machine `linux-disk.qcow2` overlay stores guest disk
changes. No machines are pre-created or started on application launch. Each
VM has its own QEMU and noVNC processes; QEMU and websockify only listen on
localhost, and the Soil API refuses foreign Host and Origin headers.

The console uses QEMU's VNC output inside noVNC; mouse and keyboard are handled
by noVNC instead of the Windows-specific MJPEG and input protocol.

## Current scope

- **Available:** start the Soil UI, create/list/start/stop/delete desktop VMs,
  boot Linux installer ISOs in QEMU with persistent disks and boot-once media,
  use the browser console, persist VM definitions and disks, install the
  `soil-control` AI skill, use KVM or TCG fallback.
- **Not yet ported:** GitHub account linking and repository app imports,
  dedicated AI-agent machines, Windows custom VM devices, host SSH tunnel
  setup, and Windows-format RAM snapshots. The UI disables unsupported
  machine types; API attempts return an explicit error.
- The proprietary Soil Desktop image and QEMU boot combination has **not yet
  been verified with real guest images** on Linux. The installer ISO boot
  workflow has a separate real-QEMU BIOS/GRUB smoke test. Windows `machine.json` records are read
  without deletion, but Linux uses its own separate QCOW2 disk overlay.
- Closing the host sends an ACPI shutdown request and then terminates any
  guest still running after a short grace period. Guest RAM state is not
  snapshotted; disk state is persistent.

## Tests and builds

```sh
python3 -m unittest discover -s app/linux/tests -v
node --check app/linux/linux-ui.js
bash app/linux/build.sh
# With QEMU, noVNC, websockify, grub-mkrescue (and its dependencies):
SOIL_ISO_E2E=1 python3 -m unittest discover -s app/linux/tests -p test_installer_iso_e2e.py -v
```

GitHub Actions workflow `.github/workflows/linux-build.yml` runs these tests,
builds a `Soil-Linux-x86_64.tar.gz` artifact and launches the host with a
health check on an Ubuntu runner. The existing stub-QEMU test checks process management, not actual guest
boot. A separate CI test boots a real GRUB-generated x86_64 ISO inside QEMU
and verifies that guest GRUB executes using the virtual serial port.
This proves optical boot, **not** that any particular distribution's installer
successfully installs a complete OS. Real distro installation is an additional
manual verification step. A stock Soil Desktop guest-boot test requires the
private Git LFS images and a suitable virtualization-capable runner.
