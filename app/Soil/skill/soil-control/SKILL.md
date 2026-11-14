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

If a command reports that Soil is not running, ask the user to open the Soil desktop app and retry. Do not edit Soil's endpoint file or call undocumented internal APIs directly. Deleting a VM permanently removes its machine data, so obtain explicit confirmation before running `vm delete`.

## Linux VM computer use

On Linux, Soil's loopback API has a documented, token-protected computer-use
surface backed by the bundled MIT-licensed vncdotool VNC automation library.
The host prints its base URL when launched; obtain the VM ID from `/api/vms`.
Read the Bearer token locally from `~/.soil/keys/computer-use.token` and never
paste it into an external service or log it. Only act on VMs the user authorized.

- GET `/api/vms/{id}/computer/screenshot` returns an image/png screenshot.
- POST `/api/vms/{id}/computer/action` accepts JSON `{"action":"click","x":120,"y":200}`;
  `{"action":"type","text":"hello"}`; `{"action":"key","key":"enter"}`;
  or `move`, `double_click`, `scroll` with screenshot-pixel coordinates.

Require the header `Authorization: Bearer <token>`. VMs must already be
running; actions return HTTP 409 instead of starting or waking a VM.
Always take a new screenshot to confirm the result after an action.
See `app/linux/README.md` for a curl example and the complete contract.
