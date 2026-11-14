#!/usr/bin/env python3
"""Diagnostic only: print selected non-secret boot scripts from Tiny Core newc initrd."""
import gzip
import os
import re
import sys

def entries(path):
    with gzip.open(path, "rb") as stream:
        data = stream.read()
    i = 0
    while i + 110 <= len(data):
        if data[i:i+6] not in (b"070701", b"070702"):
            raise ValueError(f"cpio header magic missing at {i}")
        fields = [int(data[i+6+8*j:i+14+8*j], 16) for j in range(13)]
        size, name_size = fields[6], fields[11]
        i += 110
        name = data[i:i+name_size].rstrip(b"\\x00").decode("utf-8", errors="replace")
        i = (i + name_size + 3) & ~3
        file_data = data[i:i+size]
        i = (i + size + 3) & ~3
        if name == "TRAILER!!!":
            break
        yield name, file_data, fields[1]

def main(path):
    all_entries = list(entries(path))
    print("INITRD files", len(all_entries))
    names = [name for name, _, _ in all_entries]
    for name in names:
        if re.search(r"(^|/)(init|inittab|bootlocal\\.sh|bootsync\\.sh|rcS|rc\\.local|rc\\.sysinit|sshd|dropbear|Xvesa|startx|xinitrc)$", name, re.I):
            print("BOOT_FILE", name)
    allow = [
        r"^init$", r"^etc/inittab$", r"^etc/init\\.d/rcS$",
        r"^etc/init\\.d/tc-config$", r"^opt/bootlocal\\.sh$",
        r"^opt/bootsync\\.sh$", r"^etc/rc\\.local$",
    ]
    for name, contents, mode in all_entries:
        if not any(re.match(p, name) for p in allow) or len(contents) > 25000:
            continue
        if b"PRIVATE KEY" in contents or b"ssh-rsa" in contents:
            continue
        print(">>> BEGIN", name, "bytes", len(contents))
        print(contents[:20000].decode("utf-8", errors="replace"))
        print("<<< END", name)

if __name__ == "__main__":
    main(sys.argv[1])
