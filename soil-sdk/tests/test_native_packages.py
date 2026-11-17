"""Native .sol packaging regression tests: run without KVM, loop mounts or X11."""
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

SDK = pathlib.Path(__file__).resolve().parents[1]
CLI = SDK / "tools" / "soil.py"
sys.path.insert(0, str(SDK / "tools"))
import soilmk  # noqa: E402


class NativePackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)

    def cli(self, *args, success=True):
        result = subprocess.run(
            [sys.executable, str(CLI), *map(str, args)],
            cwd=str(self.root), capture_output=True, text=True, timeout=45,
        )
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def manifest(self, package):
        hdr = soilmk.parse_header(str(package))
        self.assertEqual(hdr["kind"], 4)
        self.assertEqual(hdr["kind_name"], "native")
        self.assertEqual(hdr["digest"], soilmk.compute_digest(str(package), hdr))
        value, _ = soilmk.load_manifest(str(package), hdr)
        return value

    def test_real_linux_script_package(self):
        script = self.root / "hello.sh"
        script.write_text("#!/bin/sh\necho real-linux-app\n")
        script.chmod(0o755)
        pkg = self.root / "Hello.sol"
        self.cli("native", script, "--id", "org.soil.native-hello",
                 "--name", "Native Hello", "--arg", "test", "-o", pkg)
        m = self.manifest(pkg)
        self.assertEqual(m["entry"], "app/run")
        self.assertEqual(m["args"], ["test"])
        self.assertFalse(m["terminal"])
        self.cli("verify", pkg)

    def test_native_directory_with_resources(self):
        app = self.root / "MyAppDir"
        app.mkdir()
        (app / "AppRun").write_text("#!/bin/sh\ncat app/data.txt\n")
        (app / "AppRun").chmod(0o755)
        (app / "data.txt").write_text("real application resources\n")
        pkg = self.root / "AppDir.sol"
        self.cli("native", app, "--name", "App Directory", "--terminal", "-o", pkg)
        m = self.manifest(pkg)
        self.assertEqual(m["entry"], "app/AppRun")
        self.assertTrue(m["terminal"])
        self.assertEqual(m["id"], "org.soil.app-directory")

    def test_rejects_non_linux_payload(self):
        bad = self.root / "setup.deb"
        bad.write_bytes(b"not a runnable Linux executable")
        result = self.cli("native", bad, success=False)
        self.assertIn("native entry must be", result.stderr)

    def test_rejects_wrong_elf_architecture(self):
        fake = bytearray(64)
        fake[:4] = b"\x7fELF"
        fake[4:6] = b"\x02\x01"
        fake[16:18] = b"\x02\x00"
        fake[18:20] = b"\xb7\x00"  # AArch64, not x86_64.
        binary = self.root / "arm64.elf"
        binary.write_bytes(fake)
        self.cli("native", binary, success=False)

    def test_accepts_x86_64_type2_appimage_header(self):
        fake = bytearray(128)
        fake[:4] = b"\x7fELF"
        fake[4:6] = b"\x02\x01"
        fake[16:18] = b"\x03\x00"
        fake[18:20] = b"\x3e\x00"
        fake[8:11] = b"AI\x02"
        source = self.root / "sample.AppImage"
        source.write_bytes(fake)
        pkg = self.root / "AppImage.sol"
        self.cli("native", source, "-o", pkg)
        self.assertEqual(self.manifest(pkg)["kind"], "native")

    def test_rejects_escaping_symlink_in_directory(self):
        if os.name == "nt":
            self.skipTest("symlink privileges vary on Windows")
        directory = self.root / "linked"
        directory.mkdir()
        (directory / "AppRun").write_text("#!/bin/sh\nexit 0\n")
        (directory / "outside").symlink_to("/etc/passwd")
        self.cli("native", directory, success=False)

    def test_rejects_invalid_package_id(self):
        script = self.root / "hello"
        script.write_text("#!/bin/sh\nexit 0\n")
        result = self.cli("native", script, "--id", "../unsafe", success=False)
        self.assertIn("id must match", result.stderr)

    def test_rejects_native_without_executable_entry(self):
        folder = self.root / "bad"
        folder.mkdir()
        (folder / "soil.json").write_text(json.dumps({
            "schema": 1, "id": "org.soil.test", "name": "Bad", "version": "1.0.0",
            "kind": "native", "entry": "missing", "icon": "icon.png",
        }))
        soilmk.solid_png(str(folder / "icon.png"))
        self.cli("build", folder, success=False)


if __name__ == "__main__":
    unittest.main()
