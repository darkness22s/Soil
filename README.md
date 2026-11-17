# Soil

Soil is a local Windows host for small virtual machines and local apps. It runs
soil guest images, keeps machine state and user content under `~/.soil`, and
includes a Files view for browsing that home without leaving the app.

An early Apple Silicon macOS launcher is available under `app/macos`. It uses
QEMU software emulation for the x86_64 guest and does not yet provide the Windows
host's feature set. See [`app/macos/README.md`](app/macos/README.md) before use.

A Linux x86_64 host preview is under [`app/linux`](app/linux/README.md). It serves
`app.html` in your browser and runs guest VMs with QEMU/KVM and noVNC. It is a
separate launcher, with partial feature parity; use `bash app/linux/build.sh`
to produce the Linux tarball and see the Linux README for dependencies and
limitations.

This repository is a private build. Do not redistribute the installer or build
artifacts yet.

## Requirements

- Windows 10/11 x64
- Microsoft Edge WebView2 Runtime
- Hardware virtualization and the Windows Hypervisor Platform feature
- .NET 8 SDK for local builds
- WiX 5 for MSI builds
- MinGW-w64 (WinLibs) for the VM engine

## Layout

```
app/
  Soil/          Windows host, API, UI, and file explorer
  installer/     per-user WiX installer and bootstrap
  build.ps1      publish app, copy engine/images, build MSI
engine/          VM engine (vmhost.exe) and slirp networking
soil-sdk/        .sol packaging SDK, templates, and docs
archive/         historical prototype and private-build notes
```

## User home

The app and installer create `%USERPROFILE%\.soil` on first run:

```
~/.soil/
  config.json
  machines.json
  machines/<machine-id>/
    machine.json
    logs/vm.log
    data/
  apps/
  skills/
  cache/
  logs/
  keys/ssh/id_ed25519
  images/
  webview2/
```

On first launch, machines from the legacy `%LOCALAPPDATA%\Boxify` store are
imported into the new layout. The legacy folder is left in place. User machines
and files are never removed by an upgrade or uninstall.

## Native Linux `.sol` packages (SDK)

The SDK also has `python soil-sdk/tools/soil.py native PATH` to wrap an
x86_64 ELF, Type 2 AppImage, or prepared Linux AppDir into a signed `.sol`
package with a native executable entry, no web UI required. See
[`soil-sdk/docs/NATIVE-APPS.md`](soil-sdk/docs/NATIVE-APPS.md) for usage
and library requirements. This is **separate** from the Windows UI's
single-file VM importer below. Existing prebuilt guest images need the updated
`soil-sdk/runtime/soil.c` CLI compiled and bundled before `kind: native`
will execute inside them.

## Native Linux apps in the default Soil VM

The **Windows desktop Soil app** boots a Tiny Core Linux guest from the
**bundled** `vmlinuz64`, `core2.gz`, and `soil.squashfs` image set. Select
an ordinary desktop VM and click **Linux apps** to import a native
**x86-64 Linux ELF executable** or **Type 2 AppImage** (up to 256 MiB).
The file transfers through Soil's existing SSH port forward and is
executed **inside that guest Linux desktop**, not by the Windows host.
The VM must be running to launch it. The VM does not auto-start for an
import.

Imported binaries are stored per VM at
`~/.soil/machines/<vm-id>/native-apps/<app-id>/` on the host. The guest
launcher and desktop entries are restored on the next **explicit** VM start,
because the stock Tiny Core guest's `/home` is a temporary filesystem.
Stopping a VM stops its applications. Removing an imported app deletes the
host copy and its guest launcher; deleting the VM removes its imports.

The current guest is a **minimal Tiny Core Linux system**, not Ubuntu or
Debian. Binaries must be **x86-64 Linux executables** and their required
libraries must be available inside the guest. Type 2 AppImages use
`APPIMAGE_EXTRACT_AND_RUN=1` to avoid a FUSE requirement. **.deb**, **.rpm**,
**.iso**, ARM, Windows EXE and shell installers are not supported.
Apps downloaded from unknown sources can compromise their guest. Only
import executables you trust. Soil generates a per-install SSH key when needed, injects only its public
key into each VM's boot image, and keeps the private key in the local Soil
home. The guest SSH tunnel must be available for installation and launch.

API (local loopback only):

```text
GET    /api/vms/{id}/native-apps
POST   /api/vms/{id}/native-apps                   (Content-Type: application/octet-stream,
                                                    X-Soil-Filename: URL-encoded name)
POST   /api/vms/{id}/native-apps/{appId}/launch
DELETE /api/vms/{id}/native-apps/{appId}
```

### Bundled image build

The standard `app/build.ps1` creates the normal Windows MSI and now patches
the **published** copy of `core2.gz` with
`soil-sdk/runtime/soil-native` using
`tools/soil-ci/inject-native-guest.py`. The private original image in
`app/images` is never altered. This adds the guest launcher directly to
the initramfs while preserving the same WHPX engine, kernel, SquashFS
system disk, and app UI. Python 3 is required at build time (GitHub Actions
provisions it). Windows installer CI verifies that the launcher is present
in the packaged image.

Linux CI tests the image-injection format and guest install/launch/remove
flow. The Windows installer CI builds and checks the actual bundled assets.
**Full graphical ELF/AppImage execution within a WHPX VM still requires the
trusted self-hosted WHPX smoke runner or manual machine testing.** Neither
the hosted build nor the Linux script tests proves arbitrary third-party
binaries will run in this compact guest.

## Agent machines

The terminal button beside the machine list creates a 512 MB agent machine with
Claude Code and Codex CLI baked into the guest image. Start the machine and use
**Open tunnel** to run `claude` or `codex`. Sign in on first use; the guest home
lives on a sparse data disk at `%USERPROFILE%\.soil\machines\<id>\data\disk.img`
and survives stops and restarts; the system image stays read-only. Sign-in pages
open in the machine's built-in browser.

## Windows VM checkpoints and on-demand recall

Windows desktop machines start **only on request**. On a clean Stop, Soil
checkpoints the guest CPU, device state, and RAM to
`~/.soil/machines/<id>/resume.snap` and exits the VM engine. A parked or
stopped VM has no running `vmhost.exe` process and uses **no guest CPU or
resident VM memory**; its snapshot and persistent data disk still occupy disk
space. The Soil UI/server itself continues to use normal application resources.

On the next explicit Start, Soil consumes the checkpoint, creates a fresh WHPX
partition, restores CPU/device state, and loads guest RAM in 64-KiB chunks only
as needed. If the checkpoint is invalid, the engine falls back to a cold boot.
A one-shot in-use checkpoint is never replayed after a VM crash, because a
disk might have changed since that snapshot.

Interactive desktop VMs also **park automatically after 90 seconds with the
Soil window minimized or unfocused**, or after 90 seconds without an active
VM viewer or input request (for example, a VM left unselected while working
elsewhere in Soil). Refocusing Soil does not wake them: press Start to recall
the VM. Active app imports and AI-agent workers are
excluded from automatic focus-based parking because their ongoing background
work must not be silently interrupted; their Stop control still checkpoints
and releases the VM. Closing Soil stops all remaining VMs.

A true end-to-end boot from an exited process **cannot be guaranteed under
5 milliseconds**: host process launch, WHPX setup, snapshot I/O, network
startup, and guest readiness all have costs. The engine reports the internal
snapshot restore duration separately from end-to-end guest-ready time.

Interactive **desktop** machines now show their first *guest-drawn frame*
without waiting for a separate SSH login/desktop-process probe, including when
SSH forwarding is configured. Agent and app worker machines still wait for
service readiness. **First drawn pixels are not proof that SSH, the desktop
window manager, or every guest application is ready.**

### Fast first boot without a background VM

When you explicitly **create** a new VM on a Windows NTFS volume, Soil clones
a preformatted ext4 filesystem from one of three bundled sparse templates:
**40 GiB, 80 GiB (the default machine tier), or 160 GiB**. The compressed
templates are approximately 87/166/283 KiB, respectively, and copying the
initialized sectors requires only a few megabytes of actual disk space.
The full selected disk size remains available to the guest. There is no
guest process running during creation, and **existing data disks are never
modified**. On a filesystem without sparse-file support or if a template is
missing, the guest performs its original first-run formatting instead.

The bundled Tiny Core image also starts static SSH before extension hooks,
and initializes ephemeral zram swap without waiting to generate a random
swap UUID. These tweaks are applied when preparing the Windows installer.
Python 3 is required only to *build* the optimized initrd, not to run Soil;
a local build without Python retains the stock guest startup script.

For reproducible WHPX measurements, run this Windows x64 smoke test with
Python 3 and the bundled guest images:

```powershell
.\engine\snapshot-smoke-test.ps1 -ImagesDir .\app\images
```

The Windows 2025 CI workflow
(`.github/workflows/whpx-runtime-verification.yml`) tests actual Linux SSH
readiness, a preformatted persistent data disk, snapshot save, complete VM
engine exit, snapshot restore, and the app's Create/Start/Idle/Recall/Stop
lifecycle. A November 2026 run recorded **3.35 seconds** for a fresh SSH-ready
boot, **595 milliseconds** to restore from a parked snapshot, and **1.84
seconds** for the first guest-drawn frame in the app. The tests are per-run
measurements on nested virtualization, not guaranteed across all hardware.
**Under two seconds for complete first-ever Linux/SSH readiness is still not
achieved.** The first displayed guest frame is not proof that every application
or service is initialized.

The template generator is `engine/pack-ext4-template.py`, run on Linux after
`mkfs.ext4` prepares a regular sparse image. The checked-in templates allow
Windows installer builds and end-user creation without Linux tools.

## Build cache

Imported static apps are cached under `~/.soil/cache/builds` by source archive
hash, project path, build commands, and toolchain. Entries are immutable and can
be deleted at any time. A local warm workspace under `~/.soil/cache/workspaces`
keeps installed dependencies for repos that are rebuilt repeatedly.

Set `SOIL_BUILD_CACHE` to a shared folder to reuse build outputs across PCs. The
shared folder must be writable by every Soil instance, and all instances should
use the same toolchain versions. Set `SOIL_BUILD_CACHE_SALT` to force a separate
cache namespace when build environments intentionally differ.

## Build

```powershell
.\app\build.ps1
```

This publishes the self-contained app to `app\dist\Soil`, copies the VM engine
and guest images, and builds `app\dist\Soil-4.2.1-x64.msi`. The installer is
per-user and runs `Soil.exe --bootstrap` to create the Soil home.

To use different guest images, pass `-ImagesDir <path>` with a folder containing
`vmlinuz64`, `core2.gz`, and `soil.squashfs`. Without that option, the build
script checks `app\images`, then the existing local private staging folder.

## GitHub Actions Windows installer

Run `.github/workflows/windows-installer.yml` with **Actions > Build Soil Windows installer > Run workflow**, or push changes to the app, engine, SDK, or workflow. It builds the x64 MSI on a GitHub-hosted Windows runner and publishes it as the `Soil-Windows-x64-installer` workflow artifact (retained for 14 days). The private guest images are stored in Git LFS and checked out by the workflow. MinGW-w64 and WiX 5 are installed during the build.

## GitHub Actions VM smoke test

`.github/workflows/soil-vm-smoke.yml` builds Soil, boots the guest, and checks the
SSH and MCP-over-SSH host forwards. It is a manual workflow because guest images
are private and must already be present on the runner. It requires a trusted
self-hosted Windows x64 runner labeled `soil-whpx`, with Windows Hypervisor
Platform enabled, .NET 8, and MinGW-w64 installed. Enter the image directory and
optional `gcc` path when dispatching the workflow. Do not use this runner for
untrusted pull requests: nested virtualization is not officially supported on
GitHub-hosted runners, and self-hosted runners share the host's privileges.

## Run

```powershell
.\app\dist\Soil\Soil.exe
```

Use the Files tab to browse the Soil home. Use Settings to open the home folder.
The host API listens on loopback only.

With Soil running, control VMs from PowerShell using the installed executable:

```powershell
$soil = (Get-Process -Name Soil | Select-Object -First 1 -ExpandProperty Path)
& $soil vm list
& $soil vm create 'workbox' --size m
& $soil vm start '<vm-id>'
& $soil vm stop '<vm-id>'
```

`Soil.exe vm help` lists supported commands. The **Install Skill** button at the
bottom of Soil installs the AI-ready `soil-control` skill into detected Claude
Code, Codex, Sparky, and OpenCode user skill folders. Existing skills are never
overwritten; the button reports each installation result and destination.

## SDK

The `.sol` SDK lives in `soil-sdk`. See `soil-sdk/README.md` and
`soil-sdk/docs/` for packaging, manifest, runtime, and security details.

## Archive

`archive/` contains the earlier frontend prototype, the October 19 working
report, and the public T3Code certificate kept for historical reference. These
are not part of the Soil host build.

On Linux, running VMs also expose a token-authenticated, on-demand
[screenshot and computer-action API](app/linux/README.md#agent-computer-use-api-linux-vms)
backed by bundled MIT-licensed vncdotool (see the
[upstream license](app/linux/third-party/vncdotool-LICENSE.txt)).
