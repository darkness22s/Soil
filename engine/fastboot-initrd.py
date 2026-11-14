#!/usr/bin/env python3
"""Prepare a faster Tiny Core initrd without modifying the checked-in LFS image.

The guest configures a RAM-backed swap device before mounting its image,
setting up its persistent home and starting SSH. Its slow mkswap phase can
run concurrently with the remaining boot work; it completes before the
desktop needs swap. All other guest files and cpio metadata stay unchanged.
"""
import gzip
from pathlib import Path
import sys


def align4(value):
    return (value + 3) & ~3


def patch_bootlocal(payload):
    marker = b"SOIL_FASTBOOT_ASYNC_ZRAM"
    if marker in payload:
        return payload
    begin = b"  mkswap /dev/zram0 >/dev/null 2>&1"
    end = b'  echo "SOIL: zram up t=$(date +%s)" > /dev/ttyS0'
    if payload.count(begin) != 1 or payload.count(end) != 1:
        raise ValueError("Guest bootlocal.sh has changed; refusing an unsafe patch")
    payload = payload.replace(
        begin,
        b"  # SOIL_FASTBOOT_ASYNC_ZRAM: prepare swap during disk/extension init\\n"
        b"  (\\n" + begin,
    )
    payload = payload.replace(end, end + b"\\n  ) &")
    return payload


def patch_cpio(data):
    cursor = 0
    while cursor + 110 <= len(data):
        header = data[cursor:cursor + 110]
        if header[:6] not in (b"070701", b"070702"):
            raise ValueError(f"Invalid newc header at offset {cursor}")
        size = int(header[54:62], 16)
        name_size = int(header[94:102], 16)
        name_start = cursor + 110
        payload_start = align4(name_start + name_size)
        payload_end = payload_start + size
        next_entry = align4(payload_end)
        if next_entry > len(data) or not name_size:
            raise ValueError("Truncated cpio archive")
        name = data[name_start:name_start + name_size].rstrip(b"\\x00")
        if name == b"opt/bootlocal.sh":
            original = data[payload_start:payload_end]
            updated = patch_bootlocal(original)
            if updated == original:
                return data, False
            new_header = bytearray(header)
            new_header[54:62] = f"{len(updated):08x}".encode("ascii")
            if header[:6] == b"070702":
                new_header[102:110] = f"{sum(updated) & 0xFFFFFFFF:08x}".encode("ascii")
            pad = b"\\x00" * ((-len(updated)) % 4)
            return (data[:cursor] + bytes(new_header) +
                    data[name_start:payload_start] + updated + pad +
                    data[next_entry:]), True
        if name == b"TRAILER!!!":
            break
        cursor = next_entry
    raise ValueError("No opt/bootlocal.sh found in Tiny Core initrd")


def main():
    if len(sys.argv) != 3:
        raise SystemExit("Usage: python fastboot-initrd.py source-core.gz output-core.gz")
    source, dest = (Path(arg) for arg in sys.argv[1:])
    raw = gzip.decompress(source.read_bytes())
    patched, changed = patch_cpio(raw)
    if changed:
        result = gzip.compress(patched, compresslevel=6, mtime=0)
        # Fully verify the recompressed CPIO before publishing it.
        verified, _ = patch_cpio(gzip.decompress(result))
        if verified != patched:
            raise RuntimeError("Fastboot initrd round-trip verification failed")
        dest.parent.mkdir(parents=True, exist_ok=True)
        temp = dest.with_name(dest.name + ".fastboot.tmp")
        temp.write_bytes(result)
        temp.replace(dest)
    elif source.resolve() != dest.resolve():
        dest.write_bytes(source.read_bytes())
    print(f"Fastboot guest initrd: {'optimized' if changed else 'already optimized'} ({dest.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
