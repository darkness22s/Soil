# Soil

Soil is a local Windows host for small virtual machines and local apps. It runs
soil guest images, keeps machine state and user content under `~/.soil`, and
includes a Files view for browsing that home without leaving the app.

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

## Build

```powershell
.\app\build.ps1
```

This publishes the self-contained app to `app\dist\Soil`, copies the VM engine
and guest images, and builds `app\dist\Soil-4.2.0-x64.msi`. The installer is
per-user and runs `Soil.exe --bootstrap` to create the Soil home.

To use different guest images, pass `-ImagesDir <path>` with a folder containing
`vmlinuz64`, `core2.gz`, and `soil.squashfs`. Without that option, the build
script checks `app\images`, then the existing local private staging folder.

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

`Soil.exe vm help` lists supported commands. The Skill button at the bottom of
Soil provides an AI-ready `soil-control` skill with command examples and safety
notes.

## SDK

The `.sol` SDK lives in `soil-sdk`. See `soil-sdk/README.md` and
`soil-sdk/docs/` for packaging, manifest, runtime, and security details.

## Archive

`archive/` contains the earlier frontend prototype, the October 19 working
report, and the public T3Code certificate kept for historical reference. These
are not part of the Soil host build.
