#!/usr/bin/env python3
"""Windows CI/test-only provisioning of the bundled Soil ext4 template."""
import ctypes
import gzip
import msvcrt
from pathlib import Path
import struct
import sys

FSCTL_SET_SPARSE = 0x000900C4
MAGIC = b"SOILX4P1"
DISK_SIZE = 40 * 1024 ** 3


def main():
    if len(sys.argv) != 3:
        raise SystemExit("Usage: expand-ext4-template.py template.gz output.img")
    source, dest = map(Path, sys.argv[1:])
    if dest.exists():
        raise FileExistsError(dest)
    try:
        with dest.open("w+b", buffering=0) as output:
            handle = msvcrt.get_osfhandle(output.fileno())
            ret = ctypes.c_ulong(0)
            if not ctypes.windll.kernel32.DeviceIoControl(
                ctypes.c_void_p(handle), ctypes.c_ulong(FSCTL_SET_SPARSE),
                None, 0, None, 0, ctypes.byref(ret), None
            ):
                raise OSError("Cannot provision sparse disk on this filesystem")
            with gzip.open(source, "rb") as archive:
                if archive.read(8) != MAGIC:
                    raise ValueError("Invalid Soil ext4 template magic")
                size = struct.unpack("<Q", archive.read(8))[0]
                if size != DISK_SIZE:
                    raise ValueError("Unexpected ext4 template disk size")
                output.truncate(size)
                previous_end = 0
                while True:
                    header = archive.read(12)
                    if len(header) != 12:
                        raise EOFError("Truncated template extent header")
                    offset, count = struct.unpack("<QI", header)
                    if (offset, count) == (0, 0):
                        break
                    if count == 0 or count > 1024 * 1024 or offset < previous_end or offset + count > size:
                        raise ValueError("Invalid ext4 template extent")
                    data = archive.read(count)
                    if len(data) != count:
                        raise EOFError("Truncated ext4 template extent payload")
                    output.seek(offset)
                    output.write(data)
                    previous_end = offset + count
                if archive.read(1):
                    raise ValueError("Extra payload after ext4 template end")
            output.seek(1024 + 0x38)
            if output.read(2) != b"\x53\xef":
                raise ValueError("Copied disk lacks ext4 superblock")
        print("Prepared preformatted 40-GiB sparse ext4 volume:", dest)
    except Exception:
        dest.unlink(missing_ok=True)
        raise


if __name__ == "__main__":
    main()
