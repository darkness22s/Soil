"""Portable unit and API smoke tests for Soil's Linux host."""
import http.client
import io
import json
import os
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

from app.linux import soil


def fake_iso():
    data = bytearray(40000)
    data[32769:32774] = b"CD001"  # ISO9660 primary volume descriptor.
    return bytes(data)


class LinuxHostTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name) / ".soil"
        self.host = soil.LinuxHost(self.home)

    def test_empty_start_does_not_create_or_boot_vms(self):
        self.assertEqual(self.host.list(), [])
        self.assertFalse(self.host.running)
        self.assertFalse(self.host.index.exists())

    def test_create_persists_and_loads_without_autostart(self):
        first = self.host.create("Testing", "tiny")
        second = self.host.create("Testing", "nano")
        self.assertEqual(first["name"], "Testing")
        self.assertEqual(second["name"], "Testing 2")
        self.assertEqual(first["ramMb"], 512)
        self.assertEqual(second["ramMb"], 2048)
        self.assertEqual(first["kind"], "desktop")
        self.assertTrue((self.home / "machines" / first["id"] / "machine.json").is_file())
        another = soil.LinuxHost(self.home)
        self.assertEqual(len(another.list()), 2)
        self.assertFalse(another.running)
        self.assertTrue(another.delete(first["id"]))
        self.assertEqual(len(another.list()), 1)
        self.assertFalse((self.home / "machines" / first["id"]).exists())

    def test_invalid_creates_rejected(self):
        with self.assertRaises(soil.SoilError):
            self.host.create(" ", "tiny")
        with self.assertRaises(soil.SoilError):
            self.host.create("ok", "huge")
        self.assertFalse(self.host.safe_id("../etc/passwd"))

    def test_linux_installer_media_and_disk_persist(self):
        vm = self.host.create("Ubuntu", "standard", "linux-iso")
        self.assertEqual(vm["status"], "stopped")
        self.assertFalse(vm["installer"]["mounted"])
        self.assertEqual(vm["diskGb"], 80)
        image = fake_iso()
        imported = self.host.mount_installer(vm["id"], "ubuntu.iso", io.BytesIO(image), len(image))
        self.assertTrue(imported["installer"]["bootNext"])
        self.assertTrue(imported["installer"]["mounted"])
        self.assertEqual(self.host._installer_path(vm["id"]).read_bytes(), image)
        fresh = soil.LinuxHost(self.home)
        self.assertTrue(fresh.get(vm["id"])["installer"]["bootNext"])
        self.assertEqual(fresh.eject_installer(vm["id"])["installer"]["mounted"], False)
        self.assertFalse(fresh._installer_path(vm["id"]).exists())
        self.assertTrue(fresh.delete(vm["id"]))

    def test_linux_iso_validation_replaces_atomically(self):
        vm = self.host.create("Debian", "standard", "linux-iso")
        mid = vm["id"]
        original = fake_iso()
        self.host.mount_installer(mid, "debian.iso", io.BytesIO(original), len(original))
        for bad in (b"A" * 40000, b"short"):
            with self.assertRaises(soil.SoilError):
                self.host.mount_installer(mid, "bad.iso", io.BytesIO(bad), len(bad))
        self.assertEqual(self.host._installer_path(mid).read_bytes(), original)
        with self.assertRaises(soil.SoilError):
            self.host.mount_installer(mid, "../evil.iso", io.BytesIO(original), len(original))
        with self.assertRaises(soil.SoilError):
            self.host.create("invalid", "standard", "bad-boot")
        self.assertEqual(self.host.get(mid)["installer"]["name"], "debian.iso")

    def test_iso_vm_boots_from_virtual_cdrom_without_private_images(self):
        vm = self.host.create("Linux", "standard", "linux-iso")
        mid = vm["id"]
        with self.assertRaisesRegex(soil.SoilError, "Attach"):
            self.host.start(mid)
        iso = fake_iso()
        self.host.mount_installer(mid, "linux.iso", io.BytesIO(iso), len(iso))
        bindir = Path(self.temp.name) / "bin"
        bindir.mkdir()
        for tool in ("qemu-system-x86_64", "qemu-img", "websockify"):
            script = bindir / tool
            script.write_text("#!/bin/sh\n" + (
                'for a in "$@"; do case "$a" in *.qcow2) : > "$a";; esac; done\n'
                if tool == "qemu-img" else "exec sleep 30\n"))
            script.chmod(0o755)
        novnc = Path(self.temp.name) / "novnc"
        novnc.mkdir()
        (novnc / "vnc.html").write_text("mock noVNC")
        with mock.patch.dict(os.environ, {"PATH": str(bindir) + ":" + os.environ["PATH"]}):
            with mock.patch.object(soil, "no_vnc_root", return_value=novnc):
                with mock.patch.object(soil.subprocess, "Popen", wraps=soil.subprocess.Popen) as popen:
                    booted = self.host.start(mid)
                    self.assertEqual(booted["status"], "running")
                    self.assertFalse(booted["installer"]["bootNext"])
                    args = next(call.args[0] for call in popen.call_args_list if "qemu-system-x86_64" in call.args[0][0])
                    self.assertIn("once=d,order=c,menu=on", args)
                    self.assertIn(str(self.host._installer_path(mid)), " ".join(args))
                    self.assertNotIn("-kernel", args)
                    self.assertTrue((self.home / "machines" / mid / "data" / "linux-disk.qcow2").is_file())
                self.assertEqual(self.host.stop(mid)["status"], "stopped")
                with mock.patch.object(soil.subprocess, "Popen", wraps=soil.subprocess.Popen) as popen:
                    self.host.start(mid)
                    self.assertIn("order=c,menu=on", next(call.args[0] for call in popen.call_args_list if "qemu-system-x86_64" in call.args[0][0]))
                    self.assertNotIn("once=d,order=c,menu=on", next(call.args[0] for call in popen.call_args_list if "qemu-system-x86_64" in call.args[0][0]))
                self.assertEqual(self.host.stop(mid)["status"], "stopped")

    def test_missing_dependencies_fails_before_process_launch(self):
        mid = self.host.create("Need QEMU", "m")["id"]
        with mock.patch.object(soil.shutil, "which", return_value=None):
            with self.assertRaisesRegex(soil.SoilError, "Install Linux VM dependencies"):
                self.host.start(mid)
        self.assertFalse(self.host.running)

    def test_git_lfs_pointers_not_treated_as_images(self):
        for image in soil.IMAGE_NAMES:
            (self.home / "images" / image).write_text(
                "version https://git-lfs.github.com/spec/v1\n" + "x" * 5000)
        with self.assertRaisesRegex(soil.SoilError, "Usable guest images"):
            self.host.image_dir()

    def test_stub_qemu_lifecycle_and_console_endpoint(self):
        # Exercise process launch and cleanup without pretending to boot a real guest.
        bindir = Path(self.temp.name) / "bin"
        bindir.mkdir()
        for tool in ("qemu-system-x86_64", "qemu-img", "websockify"):
            script = bindir / tool
            script.write_text(
                "#!/bin/sh\n" +
                ('for last do :; done\n: > "$last"\n' if tool == "qemu-img" else "exec sleep 30\n"))
            script.chmod(0o755)
        novnc = Path(self.temp.name) / "novnc"
        novnc.mkdir()
        (novnc / "vnc.html").write_text("mock noVNC")
        for image in soil.IMAGE_NAMES:
            (self.home / "images" / image).write_bytes(b"A" * 4096)
        mid = self.host.create("Can start", "tiny")["id"]
        with mock.patch.dict(os.environ, {"PATH": str(bindir) + ":" + os.environ["PATH"]}):
            with mock.patch.object(soil, "no_vnc_root", return_value=novnc):
                started = self.host.start(mid)
                self.assertEqual(started["status"], "running")
                self.assertIn("/vnc.html?", self.host.console(mid))
                self.assertTrue((self.home / "machines" / mid / "data" / "linux-disk.qcow2").exists())
                self.assertEqual(self.host.stop(mid)["status"], "stopped")
                self.assertFalse(self.host.running)


class ServerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.host = soil.LinuxHost(Path(self.temp.name) / "home")
        self.server = soil.SoilServer(self.host)
        self.worker = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.worker.start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)

    def request(self, method, path, body=None, headers=None):
        connection = http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=5)
        headers = headers or {}
        if body is not None:
            body = json.dumps(body)
            headers.setdefault("Content-Type", "application/json")
        connection.request(method, path, body=body, headers=headers)
        response = connection.getresponse()
        status = response.status
        data = response.read()
        connection.close()
        return status, data

    def test_startup_health_and_real_app_html(self):
        status, data = self.request("GET", "/api/health")
        self.assertEqual(status, 200)
        self.assertTrue(json.loads(data)["ok"])
        status, data = self.request("GET", "/")
        self.assertEqual(status, 200)
        self.assertIn(b'id="newVm"', data)
        self.assertIn(b'linux-ui.js', data)
        status, data = self.request("GET", "/api/vms")
        self.assertEqual(json.loads(data), [])

    def test_http_create_and_restart_without_autostart(self):
        status, data = self.request("POST", "/api/vms", {"name": "My Linux VM", "size": "tiny"})
        self.assertEqual(status, 200)
        vm = json.loads(data)
        self.assertEqual(vm["status"], "stopped")
        status, data = self.request("GET", "/api/vms")
        self.assertEqual(len(json.loads(data)), 1)
        status, data = self.request("DELETE", "/api/vms/" + vm["id"])
        self.assertEqual(status, 200)
        self.assertTrue(json.loads(data)["deleted"])

    def test_bad_host_origin_and_traversal_blocked(self):
        status, _ = self.request("POST", "/api/vms", {"name": "bad"}, {"Origin": "http://evil.test"})
        self.assertEqual(status, 403)
        status, _ = self.request("GET", "/%2e%2e/README.md")
        self.assertEqual(status, 404)
        status, _ = self.request("GET", "/api/vms/not-an-id/console")
        self.assertEqual(status, 404)

    def test_linux_installer_http_attach_boot_eject(self):
        status, vm_data = self.request("POST", "/api/vms", {
            "name": "Ubuntu", "size": "standard", "boot": "linux-iso",
        })
        self.assertEqual(status, 200)
        vm = json.loads(vm_data)
        mid = vm["id"]
        image = fake_iso()
        conn = http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=5)
        conn.request("POST", "/api/vms/" + mid + "/installer", body=image,
                     headers={"Content-Type": "application/octet-stream",
                              "X-Soil-Filename": "ubuntu.iso"})
        response = conn.getresponse()
        self.assertEqual(response.status, 200)
        self.assertTrue(json.loads(response.read())["installer"]["bootNext"])
        conn.close()
        self.assertFalse(self.host.running)
        self.assertEqual(self.request("POST", "/api/vms/" + mid + "/installer/boot", {})[0], 200)
        self.assertEqual(self.request("DELETE", "/api/vms/" + mid + "/installer")[0], 200)
        self.assertFalse(self.host._installer_path(mid).exists())

    def test_unsupported_features_fail_clearly(self):
        status, data = self.request("POST", "/api/apps", {"repoUrl": "https://github.com/o/r"})
        self.assertEqual(status, 501)
        self.assertIn("desktop", json.loads(data)["error"])
        status, data = self.request("GET", "/api/github/repos")
        self.assertEqual(status, 501)


if __name__ == "__main__":
    unittest.main()
