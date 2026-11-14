"""Real QEMU boot test for Soil's Linux installer ISO path.

The generated bootable ISO contains GRUB, not a Linux distribution; the test
proves virtual optical boot, persistent disk creation, serial guest execution
and the browser console. Full interactive distro installation is manual.
"""
import os
import platform
import shutil
import subprocess
import tempfile
import time
import unittest
from pathlib import Path
from urllib.request import urlopen

from app.linux import soil


@unittest.skipUnless(
    os.environ.get("SOIL_ISO_E2E") == "1" and platform.system() == "Linux" and
    platform.machine() in ("x86_64", "AMD64"),
    "Requires SOIL_ISO_E2E=1 on Linux x86_64",
)
class InstallerIsoQemuE2E(unittest.TestCase):
    def test_boot_installer_media_and_persist_disk(self):
        missing = [tool for tool in ("grub-mkrescue", "qemu-img", "qemu-system-x86_64", "websockify")
                   if not shutil.which(tool)]
        if missing or soil.no_vnc_root() is None:
            self.fail("Missing ISO/QEMU dependencies: " + ", ".join(missing))
        with tempfile.TemporaryDirectory(prefix="soil-iso-e2e-") as temp:
            root = Path(temp)
            grub = root / "iso" / "boot" / "grub"
            grub.mkdir(parents=True)
            (grub / "grub.cfg").write_text(
                "serial --unit=0 --speed=115200 --word=8 --parity=no --stop=1\n"
                "terminal_input serial\n"
                "terminal_output serial\n"
                "set timeout=0\n"
                "menuentry 'Soil ISO boot smoke' {\n"
                "  echo 'SOIL_ISO_BOOT_OK'\n"
                "  sleep 20\n"
                "}\n", encoding="utf-8")
            iso = root / "boot-check.iso"
            subprocess.run(["grub-mkrescue", "-o", str(iso), str(root / "iso")],
                           check=True, capture_output=True, timeout=90)
            host = soil.LinuxHost(root / "home")
            try:
                vm = host.create("Linux from ISO", "standard", "linux-iso")
                mid = vm["id"]
                self.assertEqual(host.get(mid)["status"], "stopped")
                with iso.open("rb") as incoming:
                    attached = host.mount_installer(mid, iso.name, incoming, iso.stat().st_size)
                self.assertTrue(attached["installer"]["bootNext"])
                started = host.start(mid)
                self.assertEqual(started["status"], "running")
                self.assertFalse(started["installer"]["bootNext"])
                with urlopen(host.console(mid).split("/vnc.html")[0] + "/vnc.html", timeout=5) as response:
                    self.assertEqual(response.status, 200)
                    self.assertIn(b"noVNC", response.read())
                serial = host.home / "machines" / mid / "logs" / "serial.log"
                deadline = time.monotonic() + 45
                while time.monotonic() < deadline:
                    if serial.exists() and b"SOIL_ISO_BOOT_OK" in serial.read_bytes():
                        break
                    time.sleep(0.3)
                else:
                    log = (host.home / "machines" / mid / "logs" / "vm.log").read_text(errors="replace")
                    self.fail("Guest did not execute bootable ISO GRUB script; QEMU log: " + log[-2000:])
                disk = host.home / "machines" / mid / "data" / "linux-disk.qcow2"
                self.assertTrue(disk.is_file())
                self.assertGreater(disk.stat().st_size, 0)
                self.assertEqual(host.stop(mid)["status"], "stopped")
                self.assertFalse(host.running)
                self.assertEqual(host.boot_installer_next(mid)["installer"]["bootNext"], True)
                self.assertEqual(host.eject_installer(mid)["installer"]["mounted"], False)
                self.assertFalse(host.get(mid)["installer"]["bootNext"])
                self.assertTrue(disk.exists(), "Ejecting installer must not remove disk")
            finally:
                host.stop_all()


if __name__ == "__main__":
    unittest.main()
