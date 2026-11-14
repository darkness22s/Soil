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
sudo apt install python3 qemu-system-x86 qemu-utils novnc websockify xvfb x11vnc xauth openbox
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

## Importing Linux-native applications (x86_64)

On Linux, Soil can import a **local Linux x86_64 ELF executable or AppImage** directly.
Click **Import Linux app** below **New VM** in `app.html`, choose your
`.AppImage`, `.elf`, or `.bin` file, read the security warning, and confirm.
The app is copied to `~/.soil/apps/<id>/` (maximum 256 MiB). The import
itself does **not** launch it; confirming the import in the UI also requests
a one-time **Start**. Imported apps appear in Soil's normal machine list and
can be started, stopped, opened in the noVNC console, and deleted later.
The app processes and graphical display are created only on Start and torn
down on Stop. The imported binary and per-app home directory survive stops.

**Security: native apps run with your Linux user privileges on the HOST.**
An Xvfb display gives each app a separate graphical session, **not** a
filesystem or system-call sandbox, and **not** a VM. Import only binaries
you trust. The VNC bridge binds to localhost; local users can still connect
to localhost services, so don't use this for untrusted multi-user hosts.
No Linux app starts on Soil launch. File types are checked by ELF magic,
class and architecture, not by filename alone. No `.deb`, `.rpm`,
ARM ELF or arbitrary shell script support is promised. Some AppImages require
additional shared libraries, or FUSE support; Soil sets
`APPIMAGE_EXTRACT_AND_RUN=1` for type-2 AppImage compatibility.

This path is also available via the local HTTP API while Soil runs:

```sh
curl -X POST -H 'Content-Type: application/octet-stream' \
  -H 'X-Soil-Filename: example.AppImage' \
  --data-binary @./example.AppImage http://127.0.0.1:<SOIL_PORT>/api/apps/native
# Response contains a new stopped app ID.
curl -X POST -H 'Content-Type: application/json' -d '{}' \
  http://127.0.0.1:<SOIL_PORT>/api/vms/<ID>/start
# Then: GET /api/vms/<ID>/console, POST .../stop, DELETE /api/vms/<ID>.
```

Install native GUI dependencies from your distribution: `Xvfb`,
`x11vnc`, `xauth`, `websockify` and noVNC; `openbox` is optional
but recommended for movable windows. Guest VM images are **not** required
for native apps.

## Guest images (required for VMs, not for starting the UI)

Place **real, complete** `vmlinuz64`, `core2.gz`, and `soil.squashfs` files
in `~/.soil/images/`. Git LFS pointer files do not count. The Linux tarball
intentionally does not redistribute private guest images.

Use **New VM**, choose a resource size and click **Done** to create and start
a VM. A separate per-machine `linux-disk.qcow2` overlay stores guest disk
changes. No machines are pre-created or started on application launch. Each
VM has its own QEMU and noVNC processes; QEMU and websockify only listen on
localhost, and the Soil API refuses foreign Host and Origin headers.

The console uses QEMU's VNC output inside noVNC; mouse and keyboard are handled
by noVNC instead of the Windows-specific MJPEG and input protocol.

## Current scope

- **Available:** start the Soil UI, create/list/start/stop/delete desktop VMs,
  import/list/start/stop/delete Linux-native x86_64 ELF/AppImage applications,
  use the browser noVNC console for both, persist VM definitions and disks,
  install the `soil-control` AI skill, use KVM or TCG fallback.
- **Not yet ported:** GitHub account linking and repository app imports,
  executing arbitrary Linux apps *inside guest VMs*, dedicated AI-agent
  machines, Windows custom VM devices, host SSH tunnel setup, and
  Windows-format RAM snapshots. The UI disables unsupported
  machine types; API attempts return an explicit error.
- The guest image and QEMU boot combination has **not yet been verified with
  real guest images** on Linux. Windows `machine.json` records are read
  without deletion, but Linux uses its own separate QCOW2 disk overlay.
- Closing the host sends an ACPI shutdown request and then terminates any
  guest still running after a short grace period. Guest RAM state is not
  snapshotted; disk state is persistent.

## Tests and builds

```sh
python3 -m unittest discover -s app/linux/tests -v
node --check app/linux/linux-ui.js
bash app/linux/build.sh

# Real native-app end-to-end graphical test (Linux x86_64 + native GUI deps + GCC/libX11):
SOIL_NATIVE_E2E=1 python3 -m unittest discover -s app/linux/tests -p test_native_e2e.py -v
```

GitHub Actions workflow `.github/workflows/linux-build.yml` runs these tests,
builds a `Soil-Linux-x86_64.tar.gz` artifact and launches the host with a
health check on an Ubuntu runner. The stub-QEMU unit test checks process
management, *not* actual guest boot. The native-app CI test launches a real ELF
in Xvfb, checks the noVNC HTTP page and WebSocket handshake, and stops/cleans
up the process. That test **does not** claim guest VM boot or sandboxing.
A true guest-boot test requires the private Git LFS images and a suitable
virtualization-capable runner.
