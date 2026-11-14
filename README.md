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
snapshot restore duration separately from end-to-end guest-ready time. To
measure on a trusted Windows x64 machine with WHPX and real guest images:

```powershell
.\engine\snapshot-smoke-test.ps1 -ImagesDir .\app\images
```

This test verifies guest SSH readiness, checkpoint creation, full engine exit,
snapshot restore, and repeat checkpoint. It can also run in the
`soil-vm-smoke.yml` workflow using a self-hosted `soil-whpx` Windows runner.
These timings are not validated by GitHub's ordinary Windows build runner,
which is unsuitable for nested-virtualization measurements.

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
