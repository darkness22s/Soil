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
sudo apt install python3 python3-pip qemu-system-x86 qemu-utils novnc websockify
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
  use the browser console, persist VM definitions and disks, install the
  `soil-control` AI skill, use KVM or TCG fallback.
- **Not yet ported:** GitHub account linking and repository app imports,
  dedicated AI-agent machines, Windows custom VM devices, host SSH tunnel
  setup, and Windows-format RAM snapshots. The UI disables unsupported
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
```

GitHub Actions workflow `.github/workflows/linux-build.yml` runs these tests,
builds a `Soil-Linux-x86_64.tar.gz` artifact and launches the host with a
health check on an Ubuntu runner. The stub-QEMU unit test checks process
management, *not* actual guest boot. A true guest-boot test requires the private
Git LFS images and a suitable virtualization-capable runner.

## Agent computer use (Linux VMs)

Soil bundles the upstream [vncdotool 1.4.2](https://github.com/sibson/vncdotool)
(MIT license) for VNC/RFB screenshots and mouse/keyboard input. Soil only
provides a thin HTTP adapter and on-demand CLI; neither runs an automation
worker while idle. The original MIT license is preserved in
`app/linux/third-party/vncdotool-LICENSE.txt` (also in the tarball).

The easiest interface for local agents is the packaged `soil computer` command.
It discovers the current random API port from `~/.soil/endpoint.json` and reads
the token privately from `~/.soil/keys/computer-use.token`. Both files are mode
0600 and use `SOIL_HOME` if the host has a custom home. No manual curl headers
or port lookup are needed:

```bash
soil computer list
soil computer screenshot <vm-id> screen.png
soil computer action <vm-id> '{"action":"click","x":250,"y":150}'
soil computer action <vm-id> '{"action":"type","text":"hello world"}'
soil computer action <vm-id> '{"action":"key","key":"enter"}'
```

The same functionality is exposed for API clients:

- `GET /api/vms`: list machine IDs and statuses.
- `GET /api/vms/{id}/computer/screenshot`: `image/png`.
- `POST /api/vms/{id}/computer/action`: JSON body with `action` =
  `move`, `click`, `double_click`, `scroll`, `type`, or `key`.
  The response is `{"ok":true}`.

The computer endpoints require `Authorization: Bearer <token>` and
`Content-Type: application/json` for POST. Token and endpoint files are local
to the Soil user; never expose the service or its token publicly. Mouse
coordinates are absolute screenshot pixels. A stopped VM returns HTTP 409
without waking it. Screenshot after each action to check its effect.

```sh
BASE=$(python3 -c 'import json,pathlib; print(json.loads((pathlib.Path.home()/".soil/endpoint.json").read_text())["url"])')
VM_ID=your-32-character-vm-id
TOKEN=$(cat ~/.soil/keys/computer-use.token)
curl -fsS -H "Authorization: Bearer $TOKEN" \
    "$BASE/api/vms/$VM_ID/computer/screenshot" -o screen.png
unset TOKEN
```

Builds vendor vncdotool and its dependencies directly into the Linux tarball,
so **no installation or network fetch occurs when computer use is called**.
The Ubuntu 24.04 artifact is built with Python 3.12; build on another target
system when its Python ABI differs. Existing browser noVNC control is unchanged.
