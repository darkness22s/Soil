"""Linux installer ISO import and QEMU boot arguments for Soil desktop VMs.

Only attached guest virtual disks are writable. ISO media is always read-only.
This module intentionally uses QEMU's existing BIOS/El Torito boot support.
"""
from __future__ import annotations

import os
import uuid
from pathlib import Path

MAX_ISO_BYTES = 12 * 1024 * 1024 * 1024
MIN_ISO_BYTES = 32774


class InstallerError(Exception):
    pass


def validate_iso(image: Path) -> None:
    """Check ISO9660 volume descriptor; no execution on the host."""
    if image.stat().st_size < MIN_ISO_BYTES:
        raise InstallerError("The installer ISO is too small.")
    with image.open("rb") as stream:
        stream.seek(32769)
        if stream.read(5) != b"CD001":
            raise InstallerError("Not an ISO9660 installer image (CD001 descriptor missing).")


def save_iso(source: object, size: int, target: Path) -> None:
    """Stream a validated ISO atomically. Preserve old media on failed upload."""
    if not isinstance(size, int) or size < MIN_ISO_BYTES or size > MAX_ISO_BYTES:
        raise InstallerError("Installer ISO must be at least 32 KiB and at most 12 GiB.")
    target.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    temporary = target.with_name("." + target.name + "-" + uuid.uuid4().hex)
    try:
        with temporary.open("xb") as output:
            remaining = size
            while remaining:
                chunk = source.read(min(1024 * 1024, remaining))
                if not chunk:
                    raise InstallerError("Installer ISO upload was interrupted.")
                output.write(chunk)
                remaining -= len(chunk)
        validate_iso(temporary)
        os.replace(temporary, target)
    finally:
        temporary.unlink(missing_ok=True)


def qemu_boot_args(machine: dict, disk: Path, iso: Path) -> list[str]:
    """Boot a real x86_64 installer CD once, then boot the persistent disk.

    Both the disk and ISO remain attached during guest reboots. BootOnce is
    persisted as consumed after a successful launch by the Linux host.
    """
    if not iso.is_file() and machine.get("bootInstallerNext"):
        raise InstallerError("Attach an installer ISO before starting this Linux VM.")
    args = [
        "-drive", "file=" + str(disk) + ",format=qcow2,if=virtio",
        "-boot", "once=d,order=c,menu=on" if machine.get("bootInstallerNext") else "order=c,menu=on",
        "-vga", "std",
        "-usb", "-device", "usb-tablet",
        "-netdev", "user,id=soilnet",
        "-device", "virtio-net-pci,netdev=soilnet",
    ]
    if iso.is_file():
        args.extend(["-drive", "file=" + str(iso) + ",format=raw,media=cdrom,if=ide,readonly=on"])
    return args
