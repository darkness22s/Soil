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

If a command reports that Soil is not running, ask the user to open the Soil desktop app and retry. Do not edit Soil's endpoint file or call internal APIs directly. Deleting a VM permanently removes its machine data, so obtain explicit confirmation before running `vm delete`.
