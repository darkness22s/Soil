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
CUA_DRIVER_PATH = "bin/cua-driver"
CUA_LICENSE_PATH = "usr/share/doc/cua-driver/LICENSE.md"
XORG_DUMMY_PATH = "bin/soil-xorg/drivers/dummy_drv.so"
XORG_CONFIG_PATH = "etc/X11/soil-dummy.conf"
XORG_ENSURE_PATH = "bin/soil-ensure-desktop"
XORG_LICENSE_PATH = "usr/share/doc/xf86-video-dummy/copyright"


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


def validate_cua_driver(binary: bytes) -> None:
    """Accept only a real Linux x86_64 ELF executable, not a script or downloader."""
    if (not 65536 <= len(binary) <= 150 * 1024 * 1024 or
            binary[:6] != b"\x7fELF\x02\x01" or binary[18:20] != b"\x3e\x00"):
        raise ValueError("Cua Driver must be a Linux x86_64 ELF executable.")
    phoff = struct.unpack_from("<Q", binary, 32)[0]
    phentsize, phnum = struct.unpack_from("<HH", binary, 54)
    if phentsize < 56 or not phnum or phoff + phentsize * phnum > len(binary):
        raise ValueError("Cua Driver ELF program headers are invalid.")


def create_overlay(script: bytes, runtime: bytes | None = None,
                   cua_driver: bytes | None = None,
                   cua_license: bytes | None = None,
                   dummy_module: bytes | None = None,
                   dummy_license: bytes | None = None,
                   dummy_config: bytes | None = None,
                   desktop_script: bytes | None = None) -> bytes:
    if not script.startswith(b"#!/bin/sh\n"):
        raise ValueError("The guest launcher must be a POSIX shell script.")
    # Explicit parent directories are required by Linux's initramfs unpacker.
    # /usr/local may be remounted from soil.squashfs, so keep guest tools in
    # /bin and the Xorg driver in /bin instead.
    overlay = b""
    for inode, dirname in enumerate((
        "usr/share", "usr/share/doc", "usr/share/doc/cua-driver",
        "usr/share/doc/xf86-video-dummy", "etc/X11",
        "bin/soil-xorg", "bin/soil-xorg/drivers"
    ), 91100):
        overlay += newc_entry(dirname, b"", stat.S_IFDIR | 0o755, inode)
    overlay += newc_entry(GUEST_PATH, script, stat.S_IFREG | 0o755, 91001)
    if runtime is not None:
        validate_runtime(runtime)
        overlay += newc_entry(SOL_GUEST_PATH, runtime, stat.S_IFREG | 0o755, 91003)
    if cua_driver is not None:
        validate_cua_driver(cua_driver)
        if not cua_license or b"MIT License" not in cua_license:
            raise ValueError("The Cua Driver MIT license must be supplied.")
        overlay += newc_entry(CUA_DRIVER_PATH, cua_driver, stat.S_IFREG | 0o755, 91004)
        overlay += newc_entry(CUA_LICENSE_PATH, cua_license, stat.S_IFREG | 0o644, 91005)
    if dummy_module is not None:
        if (len(dummy_module) < 1024 or len(dummy_module) > 1024 * 1024 or
                dummy_module[:6] != b"\x7fELF\x02\x01" or
                dummy_module[18:20] != b"\x3e\x00"):
            raise ValueError("Xorg dummy module must be Linux x86_64 ELF.")
        if (not dummy_license or len(dummy_license) < 50 or
                not dummy_config or b'Driver "dummy"' not in dummy_config or
                b"1360x768" not in dummy_config or
                not desktop_script or not desktop_script.startswith(b"#!/bin/sh\n")):
            raise ValueError("Xorg module requires its license, 1360x768 config and guest startup script.")
        overlay += newc_entry(XORG_DUMMY_PATH, dummy_module, stat.S_IFREG | 0o644, 91006)
        overlay += newc_entry(XORG_LICENSE_PATH, dummy_license, stat.S_IFREG | 0o644, 91007)
        overlay += newc_entry(XORG_CONFIG_PATH, dummy_config, stat.S_IFREG | 0o644, 91008)
        overlay += newc_entry(XORG_ENSURE_PATH, desktop_script, stat.S_IFREG | 0o755, 91009)
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


def inject(source: Path, destination: Path, launcher: Path, runtime: Path | None = None,
           cua: Path | None = None, cua_license: Path | None = None,
           dummy: Path | None = None, dummy_license: Path | None = None,
           dummy_config: Path | None = None, desktop_ensure: Path | None = None) -> None:
    if source.stat().st_size < 65536:
        raise ValueError("The source core2.gz is missing (or is a Git LFS pointer).")
    contents = gzip.decompress(source.read_bytes())
    if GUEST_PATH.encode() + b"\0" in contents:
        raise ValueError("The initramfs already contains soil-native; supply unmodified image sources.")
    if runtime is not None and SOL_GUEST_PATH.encode() + b"\0" in contents:
        raise ValueError("The initramfs already contains the Soil SDK runtime.")
    if cua is not None and CUA_DRIVER_PATH.encode() + b"\0" in contents:
        raise ValueError("The initramfs already contains Cua Driver.")
    if dummy is not None and XORG_DUMMY_PATH.encode() + b"\0" in contents:
        raise ValueError("The initramfs already contains a Soil Xorg dummy module.")
    check_archive(contents)
    binary = runtime.read_bytes() if runtime is not None else None
    cua_binary = cua.read_bytes() if cua is not None else None
    license_bytes = cua_license.read_bytes() if cua_license is not None else None
    overlay = create_overlay(
        launcher.read_bytes().replace(b"\r\n", b"\n"), binary, cua_binary,
        license_bytes, dummy.read_bytes() if dummy else None,
        dummy_license.read_bytes() if dummy_license else None,
        dummy_config.read_bytes() if dummy_config else None,
        desktop_ensure.read_bytes().replace(b"\r\n", b"\n") if desktop_ensure else None)
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


def verify(image: Path, launcher: Path, runtime: Path | None = None,
           cua: Path | None = None, cua_license: Path | None = None,
           dummy: Path | None = None, dummy_license: Path | None = None,
           dummy_config: Path | None = None, desktop_ensure: Path | None = None) -> None:
    payload = gzip.decompress(image.read_bytes())
    script = launcher.read_bytes().replace(b"\r\n", b"\n")
    if (GUEST_PATH.encode() + b"\0" not in payload or script not in payload):
        raise ValueError("The bundled core2.gz does not contain the required guest native launcher.")
    if runtime is not None:
        binary = runtime.read_bytes()
        validate_runtime(binary)
        if SOL_GUEST_PATH.encode() + b"\0" not in payload or binary not in payload:
            raise ValueError("The bundled image does not contain the required static Soil SDK runtime.")
    if cua is not None:
        driver = cua.read_bytes()
        validate_cua_driver(driver)
        license_bytes = cua_license.read_bytes() if cua_license is not None else b""
        if (CUA_DRIVER_PATH.encode() + b"\0" not in payload or
                driver not in payload or
                CUA_LICENSE_PATH.encode() + b"\0" not in payload or
                license_bytes not in payload):
            raise ValueError("Bundled guest Cua Driver or MIT notice is missing.")
    if dummy is not None:
        for path, file in (
            (XORG_DUMMY_PATH, dummy),
            (XORG_LICENSE_PATH, dummy_license),
            (XORG_CONFIG_PATH, dummy_config),
            (XORG_ENSURE_PATH, desktop_ensure),
        ):
            if file is None or path.encode() + b"\0" not in payload or file.read_bytes() not in payload:
                raise ValueError(f"Bundled Xorg virtual desktop file is missing: {path}")
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
    parser.add_argument("--cua-driver", type=Path,
                        help="Verified upstream Linux x86_64 Cua Driver binary")
    parser.add_argument("--cua-license", type=Path,
                        help="Bundled upstream MIT license for the Cua Driver")
    parser.add_argument("--xorg-dummy", type=Path, help="Pinned Linux x86_64 dummy_drv.so")
    parser.add_argument("--xorg-license", type=Path, help="Upstream Xorg driver license notice")
    parser.add_argument("--xorg-config", type=Path, help="Soil 1360x768 Xorg config")
    parser.add_argument("--desktop-ensure", type=Path, help="On-demand Xorg startup script")
    parser.add_argument("--verify", action="store_true", help="Verify a patched core2.gz")
    args = parser.parse_args()
    if (args.cua_driver is None) != (args.cua_license is None):
        parser.error("--cua-driver and --cua-license must be used together")
    desktop_files = (args.xorg_dummy, args.xorg_license, args.xorg_config, args.desktop_ensure)
    if any(file is not None for file in desktop_files) and any(file is None for file in desktop_files):
        parser.error("--xorg-dummy, --xorg-license, --xorg-config and --desktop-ensure must be used together")
    if args.verify:
        verify(args.source, args.launcher, args.runtime, args.cua_driver,
               args.cua_license, *desktop_files)
    else:
        inject(args.source, args.destination, args.launcher, args.runtime,
               args.cua_driver, args.cua_license, *desktop_files)


if __name__ == "__main__":
    main()
