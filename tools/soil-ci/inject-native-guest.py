#!/usr/bin/env python3
"""Inject Soil's Linux native-app launcher into the *bundled* Tiny Core initramfs.

The normal Windows app boots vmlinuz64 + core2.gz and mounts soil.squashfs.
The initramfs is a gzip-compressed 'newc' cpio archive, which Linux supports
concatenating with an additional newc archive. Adding only this launcher keeps
the existing proprietary guest image, engine and installer layout intact.
"""
from __future__ import annotations

import argparse
import gzip
import os
import stat
import struct
import tempfile
from pathlib import Path

MAGIC = (b"070701", b"070702")
GUEST_PATH = "bin/soil-native"
SOL_GUEST_PATH = "bin/soil"


def align4(data: bytes) -> bytes:
    return data + (b"\0" * ((-len(data)) % 4))


def newc_entry(name: str, data: bytes, mode: int, inode: int) -> bytes:
    encoded = name.encode("utf-8") + b"\0"
    fields = (
        inode, mode, 0, 0, 1, 0, len(data),
        0, 0, 0, 0, len(encoded), 0,
    )
    header = b"070701" + b"".join(("%08x" % x).encode("ascii") for x in fields)
    assert len(header) == 110
    return align4(header + encoded) + align4(data)


def validate_runtime(binary: bytes) -> None:
    """Only bundle a standalone Linux x86_64 ELF; no dynamic loader dependency."""
    if (len(binary) < 64 or binary[:6] != b"\x7fELF\x02\x01" or
            binary[18:20] != b"\x3e\x00"):
        raise ValueError("Soil guest runtime must be a Linux x86_64 ELF executable.")
    if len(binary) > 16 * 1024 * 1024:
        raise ValueError("Soil guest runtime is unexpectedly large.")
    phoff = struct.unpack_from("<Q", binary, 32)[0]
    phentsize, phnum = struct.unpack_from("<HH", binary, 54)
    if phentsize < 56 or phnum == 0 or phoff + phentsize * phnum > len(binary):
        raise ValueError("Soil guest runtime has invalid ELF program headers.")
    for index in range(phnum):
        if struct.unpack_from("<I", binary, phoff + index * phentsize)[0] == 3:
            raise ValueError("Soil guest runtime must be statically linked (PT_INTERP found).")


def create_overlay(script: bytes, runtime: bytes | None = None) -> bytes:
    if not script.startswith(b"#!/bin/sh\n"):
        raise ValueError("The guest launcher must be a POSIX shell script.")
    overlay = newc_entry(GUEST_PATH, script, stat.S_IFREG | 0o755, 91001)
    if runtime is not None:
        validate_runtime(runtime)
        overlay += newc_entry(SOL_GUEST_PATH, runtime, stat.S_IFREG | 0o755, 91003)
    return overlay + newc_entry("TRAILER!!!", b"", 0, 91002)


def check_archive(cpio: bytes) -> None:
    """Fail closed for unexpected source image formats or truncated initramfs."""
    offset = 0
    trailer_seen = False
    while offset < len(cpio):
        if offset + 110 > len(cpio) or cpio[offset : offset + 6] not in MAGIC:
            if not any(cpio[offset:]):
                break
            raise ValueError("Bundled core2.gz does not contain a supported newc cpio archive.")
        fields = []
        try:
            for i in range(13):
                fields.append(int(cpio[offset + 6 + i * 8 : offset + 14 + i * 8], 16))
        except ValueError as ex:
            raise ValueError("Corrupt initramfs header.") from ex
        size, namesize = fields[6], fields[11]
        if not (1 <= namesize <= 4096):
            raise ValueError("Corrupt initramfs entry name length.")
        end_name = offset + 110 + namesize
        if end_name > len(cpio) or cpio[end_name - 1] != 0:
            raise ValueError("Truncated initramfs file name.")
        name = cpio[offset + 110 : end_name - 1]
        offset = (end_name + 3) & ~3
        if offset + size > len(cpio):
            raise ValueError("Truncated initramfs file content.")
        offset = (offset + size + 3) & ~3
        if name == b"TRAILER!!!":
            trailer_seen = True
            # Tiny Core may pad the archive with 512-byte zero blocks.
            if any(cpio[offset:]):
                raise ValueError("Unexpected nonzero bytes after the initramfs trailer.")
            break
    if not trailer_seen:
        raise ValueError("Source initramfs lacks the cpio trailer.")


def inject(source: Path, destination: Path, launcher: Path, runtime: Path | None = None) -> None:
    if source.stat().st_size < 65536:
        raise ValueError("The source core2.gz is missing (or is a Git LFS pointer).")
    contents = gzip.decompress(source.read_bytes())
    if GUEST_PATH.encode() + b"\0" in contents:
        raise ValueError("The initramfs already contains soil-native; supply unmodified image sources.")
    if runtime is not None and SOL_GUEST_PATH.encode() + b"\0" in contents:
        raise ValueError("The initramfs already contains the Soil SDK runtime.")
    check_archive(contents)
    binary = runtime.read_bytes() if runtime is not None else None
    overlay = create_overlay(launcher.read_bytes().replace(b"\r\n", b"\n"), binary)
    # Each cpio archive must start on a 4-byte boundary.
    image = contents + b"\0" * ((-len(contents)) % 4) + overlay
    output = gzip.compress(image, compresslevel=6, mtime=0)
    destination.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".soil-core-", suffix=".gz", dir=destination.parent)
    try:
        with os.fdopen(fd, "wb") as file:
            file.write(output)
        os.replace(temporary, destination)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    print(f"Embedded native Linux launcher in {destination} ({len(output)} bytes)")


def verify(image: Path, launcher: Path, runtime: Path | None = None) -> None:
    payload = gzip.decompress(image.read_bytes())
    script = launcher.read_bytes().replace(b"\r\n", b"\n")
    if (GUEST_PATH.encode() + b"\0" not in payload or script not in payload):
        raise ValueError("The bundled core2.gz does not contain the required guest native launcher.")
    if runtime is not None:
        binary = runtime.read_bytes()
        validate_runtime(binary)
        if SOL_GUEST_PATH.encode() + b"\0" not in payload or binary not in payload:
            raise ValueError("The bundled image does not contain the required static Soil SDK runtime.")
    if len(payload) <= 65536:
        raise ValueError("The bundled guest image is unexpectedly small.")
    print(f"Verified native launcher and Soil SDK runtime in {image}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="Original bundled core2.gz")
    parser.add_argument("destination", type=Path, help="Patched initramfs output")
    parser.add_argument("--launcher", type=Path, default=Path(__file__).resolve().parents[2] /
                        "soil-sdk" / "runtime" / "soil-native")
    parser.add_argument("--runtime", type=Path,
                        help="Static x86_64 Linux ELF produced by build-guest-cli.sh")
    parser.add_argument("--verify", action="store_true", help="Verify a patched core2.gz")
    args = parser.parse_args()
    if args.verify:
        verify(args.source, args.launcher, args.runtime)
    else:
        inject(args.source, args.destination, args.launcher, args.runtime)


if __name__ == "__main__":
    main()
