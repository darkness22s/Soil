#!/usr/bin/env python3
"""Repack Soil's Tiny Core initrd to bring up static SSH before optional boot work.

Uses the existing guest scripts, retaining every service and init action. The
writable per-machine home disk and zram swap are ready before SSH so existing
authorized keys and disk initialization remain unchanged. The application archive is mounted before invoking dropbear. No kernel,
filesystem, or guest image in git is modified; package an optimized copy at
build time instead.
"""
import gzip
from pathlib import Path
import sys


def aligned(value):
    return (value + 3) & ~3


def exactly_one(blob, marker):
    if blob.count(marker) != 1:
        raise ValueError(f"Unsupported guest bootlocal script: {marker!r}")
    return blob.index(marker)


def optimize_bootlocal(script):
    if b"SOIL_FASTBOOT_EARLY_SSH" in script:
        return script

    # Swap setup need not gate an authenticated SSH connection: the guest
    # has a full RAM allocation, and this block runs synchronously before
    # nonessential extension hooks and desktop startup. Never run it
    # concurrently with the writable-home filesystem initialization.
    zram_begin = exactly_one(script, b"# zram is builtin; mknod")
    zram_end = exactly_one(script, b"mount -t tmpfs -o size=192m")
    zram = script[zram_begin:zram_end]
    if b"mkswap /dev/zram0" not in zram or b"swapon /dev/zram0" not in zram:
        raise ValueError("Guest swap initialization has changed")
    script = script[:zram_begin] + script[zram_end:]

    net_begin = exactly_one(script, b"# network: PCI RTL8029")
    net_end = exactly_one(script, b"# remote-control services:")
    network = script[net_begin:net_end]
    if b"TM net-done" not in network or b"10.0.2.15" not in network:
        raise ValueError("Guest static network setup has changed")
    script = script[:net_begin] + script[net_end:]

    ssh_begin = exactly_one(script, b"/usr/local/sbin/dropbear -R -s -p 22")
    ssh_end = exactly_one(script, b'echo "SOIL: sshd up t=')
    ssh_line_end = script.index(b"\n", ssh_end) + 1
    if ssh_end < ssh_begin:
        raise ValueError("Guest SSH setup order changed")
    ssh = script[ssh_begin:ssh_line_end]
    script = script[:ssh_begin] + script[ssh_line_end:]

    home_end = exactly_one(script, b"TM home-done\n") + len(b"TM home-done\n")
    hooks_begin = exactly_one(script, b"# tce.installed hooks")
    if hooks_begin < home_end:
        raise ValueError("Guest SSH cannot start before persistent home mount")

    early = (b"# SOIL_FASTBOOT_EARLY_SSH: stable home and static image are mounted.\n"
             b"# Start the remote service before optional swap and extension hooks.\n"
             b"mkdir -p /etc/dropbear\n" + network + ssh + zram)
    # Retain the original mke2fs flags for machines without a preformatted
    # disk template; existing disks are never reformatted or truncated.
    return script[:home_end] + early + script[home_end:]


def optimize_cpio(data):
    offset = 0
    while offset + 110 <= len(data):
        hdr = data[offset:offset + 110]
        if hdr[:6] not in (b"070701", b"070702"):
            raise ValueError(f"Invalid newc archive at offset {offset}")
        size = int(hdr[54:62], 16)
        namesize = int(hdr[94:102], 16)
        name_beg = offset + 110
        data_beg = aligned(name_beg + namesize)
        data_end = data_beg + size
        next_header = aligned(data_end)
        if not namesize or next_header > len(data):
            raise ValueError("Truncated cpio record")
        name = data[name_beg:name_beg + namesize].rstrip(b"\0")
        if name == b"opt/bootlocal.sh":
            original = data[data_beg:data_end]
            patched = optimize_bootlocal(original)
            if patched == original:
                return data, False
            next_hdr = bytearray(hdr)
            next_hdr[54:62] = f"{len(patched):08x}".encode("ascii")
            if hdr[:6] == b"070702":
                next_hdr[102:110] = f"{sum(patched) & 0xFFFFFFFF:08x}".encode("ascii")
            padding = b"\0" * ((-len(patched)) % 4)
            updated = (data[:offset] + bytes(next_hdr) + data[name_beg:data_beg] +
                       patched + padding + data[next_header:])
            # Keep original filename, file mode, times, ownership and every
            # other cpio record untouched. No snapshot persistence semantics change.
            return updated, True
        if name == b"TRAILER!!!":
            break
        offset = next_header
    raise ValueError("Guest initrd lacks opt/bootlocal.sh")


def main():
    if len(sys.argv) != 3:
        raise SystemExit("Usage: fastboot-initrd.py input.gz output.gz")
    source, dest = [Path(s) for s in sys.argv[1:]]
    data = gzip.decompress(source.read_bytes())
    repacked, changed = optimize_cpio(data)
    if changed:
        compressed = gzip.compress(repacked, compresslevel=6, mtime=0)
        check, again = optimize_cpio(gzip.decompress(compressed))
        if check != repacked or again:
            raise RuntimeError("Boot image rewrite is not idempotent")
    else:
        compressed = source.read_bytes()
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_suffix(dest.suffix + ".fastboot.tmp")
    tmp.write_bytes(compressed)
    tmp.replace(dest)
    print("FASTBOOT:", "repacked" if changed else "already optimized",
          str(dest), "size", len(compressed))


if __name__ == "__main__":
    main()
