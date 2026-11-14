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


def elf_image(architecture=62):
    header = bytearray(64)
    header[:4] = b"\x7fELF"
    header[4] = 2  # ELF64
    header[5] = 1  # little-endian
    header[18:20] = architecture.to_bytes(2, "little")
    return bytes(header)


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

    def test_native_import_stays_stopped_and_persists(self):
        binary = elf_image()
        vm = self.host.create_native("Trusted Demo.AppImage", io.BytesIO(binary), len(binary))
        path = self.home / "apps" / vm["id"] / "app.AppImage"
        self.assertTrue(path.is_file())
        self.assertTrue(path.stat().st_mode & 0o100)
        self.assertEqual(vm["kind"], "app")
        self.assertEqual(vm["status"], "stopped")
        self.assertFalse(self.host.running)
        fresh = soil.LinuxHost(self.home)
        self.assertEqual(fresh.get(vm["id"])["status"], "stopped")
        self.assertEqual(fresh.get(vm["id"])["app"]["status"], "stopped")
        self.assertTrue(fresh.delete(vm["id"]))
        self.assertFalse(path.parent.exists())

    def test_native_upload_rejects_non_elf_wrong_arch_and_traversal(self):
        for name, content in [
            ("demo.AppImage", b"#! /bin/sh\n"),
            ("wrong.elf", elf_image(183)),
        ]:
            with self.assertRaises(soil.native_apps.NativeAppError):
                self.host.create_native(name, io.BytesIO(content), len(content))
        with self.assertRaises(soil.SoilError):
            self.host.create_native("../demo.elf", io.BytesIO(elf_image()), 64)
        self.assertEqual(self.host.list(), [])
        self.assertFalse(list((self.home / "apps").iterdir()))

    def test_native_launch_reuses_vm_controls_and_stops_processes(self):
        vm = self.host.create_native("Demo.elf", io.BytesIO(elf_image()), 64)
        app = mock.Mock()
        app.poll.return_value = None
        tmp = tempfile.TemporaryDirectory()
        runtime = dict(app=app, websocket_port=15940, started=soil.time.monotonic(),
                       temporary=tmp, log=io.BytesIO())
        with mock.patch.object(soil.native_apps, "start", return_value=runtime) as launch:
            self.assertEqual(self.host.start(vm["id"])["status"], "running")
            self.assertEqual(self.host.start(vm["id"])["status"], "running")
            self.assertEqual(launch.call_count, 1)
            self.assertIn(":15940/vnc.html?", self.host.console(vm["id"]))
            self.assertEqual(self.host.stop(vm["id"])["status"], "stopped")
        self.assertTrue(app.terminate.called)
        self.assertEqual(self.host.running, {})

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

    def test_native_http_binary_upload_start_and_delete(self):
        conn = http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=5)
        conn.request("POST", "/api/apps/native", body=elf_image(),
                     headers={"Content-Type": "application/octet-stream",
                              "X-Soil-Filename": "Demo.AppImage"})
        response = conn.getresponse()
        self.assertEqual(response.status, 200)
        vm = json.loads(response.read())
        conn.close()
        self.assertEqual(vm["status"], "stopped")
        self.assertFalse(self.host.running)
        status, data = self.request("GET", "/api/vms")
        self.assertEqual(json.loads(data)[0]["kind"], "app")
        status, data = self.request("DELETE", "/api/vms/" + vm["id"])
        self.assertEqual(status, 200)
        self.assertFalse((self.host.home / "apps" / vm["id"]).exists())

    def test_native_http_rejects_invalid_type(self):
        status, data = self.request("POST", "/api/apps/native", {"name": "bad"})
        self.assertEqual(status, 415)
        self.assertIn("octet-stream", json.loads(data)["error"])

    def test_unsupported_features_fail_clearly(self):
        status, data = self.request("POST", "/api/apps", {"repoUrl": "https://github.com/o/r"})
        self.assertEqual(status, 501)
        self.assertIn("desktop", json.loads(data)["error"])
        status, data = self.request("GET", "/api/github/repos")
        self.assertEqual(status, 501)


if __name__ == "__main__":
    unittest.main()
