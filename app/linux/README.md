# Soil for Linux (x86_64 preview)

Soil now has a Linux-native **host launcher**, separate from its Windows
WinForms/WebView2 + WHPX engine. It starts the actual Soil \`app.html\` UI in
your browser, manages manually created VMs under \`~/.soil\`, runs QEMU with KVM
when available (otherwise multi-threaded TCG), and embeds the guest console via
noVNC. A native Windows executable or Wine is **not** involved.

## Linux prerequisites

On Ubuntu or Debian:

\`\`\`sh
sudo apt update
sudo apt install python3 qemu-system-x86 qemu-utils novnc websockify
\`\`\`

Run from the source checkout:

\`\`\`sh
python3 app/linux/soil.py --check
python3 app/linux/soil.py
\`\`\`

Or build and install the portable tarball:

\`\`\`sh
bash app/linux/build.sh
tar -xzf app/linux/dist/Soil-Linux-x86_64.tar.gz
./Soil-Linux/install.sh
~/.local/bin/soil
\`\`\`

The launcher opens your default browser at a loopback-only, random-port URL.
To run without opening a browser, pass \`--no-browser\`. Use Ctrl+C to stop the
host and its running VMs. For faster VMs, enable hardware virtualization and
grant your user access to \`/dev/kvm\` (usually membership in the \`kvm\` group;
log out/in after changing groups).

## Guest images (required for VMs, not for starting the UI)

Place **real, complete** \`vmlinuz64\`, \`core2.gz\`, and \`soil.squashfs\` files
in \`~/.soil/images/\`. Git LFS pointer files do not count. The Linux tarball
intentionally does not redistribute private guest images.

Use **New VM**, choose a resource size and click **Done** to create and start
a VM. A separate per-machine \`linux-disk.qcow2\` overlay stores guest disk
changes. No machines are pre-created or started on application launch. Each
VM has its own QEMU and noVNC processes; QEMU and websockify only listen on
localhost, and the Soil API refuses foreign Host and Origin headers.

The console uses QEMU's VNC output inside noVNC; mouse and keyboard are handled
by noVNC instead of the Windows-specific MJPEG and input protocol.

## Current scope

- **Available:** start the Soil UI, create/list/start/stop/delete desktop VMs,
  use the browser console, persist VM definitions and disks, install the
  \`soil-control\` AI skill, use KVM or TCG fallback.
- **Not yet ported:** GitHub account linking and repository app imports,
  dedicated AI-agent machines, Windows custom VM devices, host SSH tunnel
  setup, and Windows-format RAM snapshots. The UI disables unsupported
  machine types; API attempts return an explicit error.
- The guest image and QEMU boot combination has **not yet been verified with
  real guest images** on Linux. Windows \`machine.json\` records are read
  without deletion, but Linux uses its own separate QCOW2 disk overlay.
- Closing the host sends an ACPI shutdown request and then terminates any
  guest still running after a short grace period. Guest RAM state is not
  snapshotted; disk state is persistent.

## Tests and builds

\`\`\`sh
python3 -m unittest discover -s app/linux/tests -v
node --check app/linux/linux-ui.js
bash app/linux/build.sh
\`\`\`

GitHub Actions workflow \`.github/workflows/linux-build.yml\` runs these tests,
builds a \`Soil-Linux-x86_64.tar.gz\` artifact and launches the host with a
health check on an Ubuntu runner. The stub-QEMU unit test checks process
management, *not* actual guest boot. A true guest-boot test requires the private
Git LFS images and a suitable virtualization-capable runner.
