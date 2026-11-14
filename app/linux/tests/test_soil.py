"""Portable unit and API smoke tests for Soil's Linux host."""
import http.client
import json
import os
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

from app.linux import soil


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


    def test_computer_use_token_created_private(self):
        token = self.host.home / "keys" / "computer-use.token"
        self.assertTrue(token.is_file())
        self.assertEqual(token.stat().st_mode & 0o077, 0)
        self.assertEqual(self.host.computer_token, soil.LinuxHost(self.home).computer_token)

    def test_computer_use_action_uses_upstream_vncdo(self):
        from app.linux import computer_use
        commands = computer_use.action_command({
            "action": "click", "x": 300, "y": 250, "button": "right"
        })
        self.assertEqual(commands, ["move", "300", "250", "click", "3"])
        self.assertEqual(computer_use.action_command(
            {"action": "key", "key": "ctrl-c"}), ["key", "ctrl-c"])
        with self.assertRaises(computer_use.ComputerUseError):
            computer_use.action_command({"action": "click", "x": -1, "y": 2})
        with self.assertRaises(computer_use.ComputerUseError):
            computer_use.action_command({"action": "type", "text": ""})
        with mock.patch.object(computer_use.subprocess, "run") as run:
            run.return_value.returncode = 0
            computer_use.perform(5993, {"action": "type", "text": "hi"})
            argv = run.call_args.args[0]
            self.assertIn("vncdotool.command", argv)
            self.assertIn("127.0.0.1::5993", argv)
            self.assertEqual(argv[-2:], ["type", "hi"])
            self.assertFalse(run.call_args.kwargs.get("shell", False))
            self.assertEqual(run.call_args.kwargs["timeout"], 12)


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

    def test_unsupported_features_fail_clearly(self):
        status, data = self.request("POST", "/api/apps", {"repoUrl": "https://github.com/o/r"})
        self.assertEqual(status, 501)
        self.assertIn("desktop", json.loads(data)["error"])
        status, data = self.request("GET", "/api/github/repos")
        self.assertEqual(status, 501)


    def test_computer_use_requires_token_and_running_vm(self):
        from app.linux import computer_use
        vm = self.host.create("Agent VM", "tiny")
        mid = vm["id"]
        screenshot_url = "/api/vms/" + mid + "/computer/screenshot"
        action_url = "/api/vms/" + mid + "/computer/action"
        status, _ = self.request("GET", screenshot_url)
        self.assertEqual(status, 401)
        status, _ = self.request("POST", action_url, {"action": "click", "x": 1, "y": 2})
        self.assertEqual(status, 401)
        headers = {"Authorization": "Bearer " + self.host.computer_token}
        status, _ = self.request("GET", screenshot_url, headers=headers)
        self.assertEqual(status, 409)
        status, _ = self.request("POST", action_url, {"action": "type", "text": "hi"}, headers)
        self.assertEqual(status, 409)
        self.assertFalse(self.host.running)

        class FakeProc:
            def poll(self):
                return None
        self.host.running[mid] = {"qemu": FakeProc(), "vnc_port": 5999}
        try:
            image = b"\x89PNG\r\n\x1a\n" + b"fake png"
            with mock.patch.object(soil, "computer_screenshot", return_value=image) as capture:
                status, data = self.request("GET", screenshot_url, headers=headers)
                self.assertEqual(status, 200)
                self.assertEqual(data, image)
                capture.assert_called_once_with(5999)
            with mock.patch.object(soil, "computer_perform") as act:
                status, data = self.request("POST", action_url,
                                            {"action": "click", "x": 10, "y": 20}, headers)
                self.assertEqual(status, 200)
                self.assertTrue(json.loads(data)["ok"])
                act.assert_called_once_with(5999, {"action": "click", "x": 10, "y": 20})
            with mock.patch.object(soil, "computer_perform",
                                   side_effect=computer_use.ComputerUseError(400, "bad input")):
                status, data = self.request("POST", action_url, {"action": "wrong"}, headers)
                self.assertEqual(status, 400)
                self.assertIn("bad input", json.loads(data)["error"])
        finally:
            self.host.running.pop(mid)


if __name__ == "__main__":
    unittest.main()
