#!/usr/bin/env python3
"""Pack an ext4 sparse disk into self-describing compressed extents.

Run only at build time on Linux, after mke2fs has initialized a regular sparse
40/80/160-GiB file. At runtime the Soil host expands only allocated extents, marks the
destination sparse, then atomically publishes it before launching the guest.
"""
import errno
import gzip
import os
from pathlib import Path
import struct
import sys

MAGIC = b"SOILX4P1"
RECORD_SIZE = 1024 * 1024


def main():
    if len(sys.argv) != 3:
        raise SystemExit("Usage: pack-ext4-template.py raw-disk.img sparse-template.gz")
    source = Path(sys.argv[1])
    target = Path(sys.argv[2])
    size = source.stat().st_size
    if size not in (40 * 1024 ** 3, 80 * 1024 ** 3, 160 * 1024 ** 3):
        raise ValueError(f"Only 40, 80, or 160-GiB disks are supported, got {size} bytes")
    total = 0
    segments = 0
    with source.open("rb", buffering=0) as disk, gzip.open(target, "wb", compresslevel=3) as out:
        out.write(MAGIC)
        out.write(struct.pack("<Q", size))
        offset = 0
        while offset < size:
            try:
                start = os.lseek(disk.fileno(), offset, os.SEEK_DATA)
            except OSError as ex:
                if ex.errno == errno.ENXIO:
                    break
                raise
            end = os.lseek(disk.fileno(), start, os.SEEK_HOLE)
            if start < offset or end <= start or end > size:
                raise ValueError("Invalid sparse extent returned by filesystem")
            offset = end
            pos = start
            while pos < end:
                disk.seek(pos)
                chunk = disk.read(min(RECORD_SIZE, end - pos))
                if not chunk:
                    raise IOError("Premature EOF within initialized filesystem data")
                # Do not waste archive space or runtime writes on allocated zeros.
                if any(chunk):
                    out.write(struct.pack("<QI", pos, len(chunk)))
                    out.write(chunk)
                    total += len(chunk)
                    segments += 1
                pos += len(chunk)
        out.write(struct.pack("<QI", 0, 0))
    print(f"Preformatted ext4 template: {size} logical bytes; "
          f"{segments} extents; {total} stored bytes; "
          f"{target.stat().st_size} compressed bytes")


if __name__ == "__main__":
    main()
