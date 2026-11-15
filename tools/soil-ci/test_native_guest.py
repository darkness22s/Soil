"""Tests for the *bundled* Windows guest initramfs patcher."""
import gzip
import importlib.util
import os
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
spec = importlib.util.spec_from_file_location(
    "inject_native_guest", HERE / "inject-native-guest.py"
)
injector = importlib.util.module_from_spec(spec)
spec.loader.exec_module(injector)
LAUNCHER = ROOT / "soil-sdk" / "runtime" / "soil-native"


class ImageInjectionTests(unittest.TestCase):
    def test_bundled_initramfs_overlay_is_valid_and_idempotence_checked(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            original = root / "original.gz"
            destination = root / "patched.gz"
            # Compress-resistant padding models a real initramfs, not an LFS pointer.
            archive = (
                injector.newc_entry("usr/local/bin/seed", os.urandom(150_000),
                                    stat.S_IFREG | 0o755, 1)
                + injector.newc_entry("TRAILER!!!", b"", 0, 2)
            )
            injector.check_archive(archive)
            original.write_bytes(gzip.compress(archive))
            injector.inject(original, destination, LAUNCHER)
            self.assertNotEqual(original.read_bytes(), destination.read_bytes())
            self.assertIn(injector.GUEST_PATH.encode() + b"\0",
                          gzip.decompress(destination.read_bytes()))
            injector.verify(destination, LAUNCHER)
            with self.assertRaisesRegex(ValueError, "already contains"):
                injector.inject(destination, destination, LAUNCHER)

    def test_rejects_invalid_pointer_and_truncated_newc(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            orig = root / "src.gz"
            dest = root / "dst.gz"
            orig.write_text("version https://git-lfs.github.com/spec/v1\n")
            with self.assertRaisesRegex(ValueError, "Git LFS pointer"):
                injector.inject(orig, dest, LAUNCHER)
            orig.write_bytes(gzip.compress(b"not-cpio" + os.urandom(75000)))
            with self.assertRaisesRegex(ValueError, "newc"):
                injector.inject(orig, dest, LAUNCHER)
            self.assertFalse(dest.exists())

    def test_guest_launcher_installs_launches_and_removes(self):
        # Runs only on POSIX/Linux; no guest executable runs on the Windows host.
        if os.name == "nt":
            self.skipTest("POSIX shell test is exercised on Linux CI")
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            binary = root / "smoke-app"
            binary.write_text("#!/bin/sh\nsleep 4\n")
            binary.chmod(0o755)
            env = dict(os.environ, HOME=str(root), DISPLAY=":0")
            def run(*args):
                return subprocess.run(["sh", str(LAUNCHER), *args], env=env,
                                      capture_output=True, text=True, timeout=10)
            installed = run("install", "sample123", str(binary), "Sample app")
            self.assertEqual(installed.returncode, 0, installed.stderr)
            self.assertTrue((root / ".local/share/soil/native/sample123/app").is_file())
            self.assertIn("Name=Sample app",
                          (root / ".local/share/applications/soil-native-sample123.desktop")
                          .read_text())
            launched = run("launch", "sample123")
            self.assertEqual(launched.returncode, 0, launched.stderr)
            self.assertIn("sample123", run("list").stdout)
            self.assertEqual(run("remove", "sample123").returncode, 0)
            self.assertEqual(run("list").stdout.strip(), "")
            self.assertNotEqual(run("install", "../escape", str(binary)).returncode, 0)


if __name__ == "__main__":
    unittest.main()
