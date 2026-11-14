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
import executables you trust. The host needs its existing guest SSH private
key and forward for transfer and installation.

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
