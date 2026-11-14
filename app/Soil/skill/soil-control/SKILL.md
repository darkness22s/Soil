---
name: soil-control
description: Operate the Soil desktop app and its virtual machines using the installed Soil.exe command line. Load when listing, creating, starting, stopping, or deleting Soil VMs.
---

# Soil command-line control

Use Soil's installed executable for VM operations. The Soil desktop app must be open; the CLI sends commands to its loopback-only API. In PowerShell, resolve the executable from the running Soil process:

```powershell
$soil = (Get-Process -Name Soil | Select-Object -First 1 -ExpandProperty Path)
& $soil vm list
```

If `$soil` is empty, ask the user to open Soil. In Command Prompt or scripts, use the full path to the installed `Soil.exe` and quote it when it contains spaces.

## Commands

```powershell
& $soil vm list                         # list VMs and their IDs/status
& $soil vm create 'workbox'             # create a medium (1 GB) VM
& $soil vm create 'tiny' --size s       # small (512 MB)
& $soil vm create 'large' --size l       # large (2 GB)
& $soil vm start '<vm-id>'
& $soil vm stop '<vm-id>'
& $soil vm delete '<vm-id>'             # destructive: confirm with the user first
```

`vm list` and successful mutations print JSON to stdout. Errors are written to stderr and return a nonzero exit code. Use the `id` returned by `vm list` or `vm create` for start/stop/delete; names are not accepted in place of IDs. Sizes are `s`, `m` (default), or `l`.

## Native Linux apps inside an existing desktop VM

In the **default Windows Soil app**, select a desktop VM and click **Linux
apps** next to the VM controls. Import an **x86-64 Linux ELF** executable
or **Type 2 AppImage** (up to 256 MiB). The file is stored with the VM,
transferred through Soil's existing guest SSH tunnel and launched **inside the
bundled Linux guest**. Import does not start the VM; press **Start** before
choosing **Launch**. Imports are restored into the Linux desktop when a stopped
VM is started again, but the guest processes do not auto-run.

Native ELF binaries need compatible shared libraries inside Soil's minimal
Tiny Core guest. This is not a Ubuntu/Debian package manager: `.deb`, `.rpm`,
`.iso`, Windows EXEs, and shell install scripts are not supported. The
native-application feature requires the bundled updated `core2.gz` image;
Soil creates the guest SSH key automatically when needed. Only run binaries the user explicitly chose
and trusts.

If a command reports that Soil is not running, ask the user to open the Soil desktop app and retry. Do not edit Soil's endpoint file or call internal APIs directly. Deleting a VM permanently removes its machine data, so obtain explicit confirmation before running `vm delete`.
