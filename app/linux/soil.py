#!/usr/bin/env python3
"""Soil Linux host: browser shell, persistent QEMU machines, and loopback API.

This is intentionally separate from the Windows WHPX/WinForms implementation.
It uses only Python's standard library; QEMU, noVNC and websockify are installed
as system packages. No VM is started automatically on launch.
"""
from __future__ import annotations

import argparse
import json
import mimetypes
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import uuid
import webbrowser
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlsplit

try:
    from . import native_apps
except ImportError:  # Direct execution: python3 app/linux/soil.py
    import native_apps

HERE = Path(__file__).resolve().parent
UI_DIR = HERE / "ui" if (HERE / "ui" / "app.html").is_file() else HERE.parent / "Soil" / "ui"
SKILL_FILE = (HERE / "skill" / "soil-control" / "SKILL.md"
              if (HERE / "skill" / "soil-control" / "SKILL.md").is_file()
              else HERE.parent / "Soil" / "skill" / "soil-control" / "SKILL.md")
SIZES = {
    "s": (512, 40, 1), "tiny": (512, 40, 1),
    "nano": (2048, 80, 1), "m": (1024, 80, 1),
    "standard": (4096, 80, 2), "l": (2048, 160, 2),
    "pro": (8192, 160, 4), "wide": (16384, 160, 8),
}
IMAGE_NAMES = ("vmlinuz64", "core2.gz", "soil.squashfs")


class SoilError(Exception):
    pass


def utcnow() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def atomic_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
    try:
        tmp.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
        os.replace(tmp, path)
    finally:
        tmp.unlink(missing_ok=True)


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def no_vnc_root() -> Path | None:
    candidates = [
        os.getenv("SOIL_NOVNC_DIR", ""),
        "/usr/share/novnc",
        "/usr/share/noVNC",
        "/usr/local/share/novnc",
    ]
    for candidate in candidates:
        if candidate and (Path(candidate) / "vnc.html").is_file():
            return Path(candidate).resolve()
    return None


class LinuxHost:
    def __init__(self, home: Path):
        self.home = home.expanduser().resolve()
        self.index = self.home / "machines.json"
        self.lock = threading.RLock()
        for name in ("machines", "images", "apps", "skills", "cache", "logs", "keys"):
            (self.home / name).mkdir(parents=True, exist_ok=True)
        self.machines: dict[str, dict] = {}
        self.running: dict[str, dict] = {}
        if self.index.exists():
            try:
                for item in json.loads(self.index.read_text(encoding="utf-8")).get("vms", []):
                    if isinstance(item, dict) and self.safe_id(item.get("id")):
                        self.machines[item["id"]] = item
            except (ValueError, OSError, TypeError) as exc:
                raise SoilError("Could not read existing machines.json: " + str(exc)) from exc

    @staticmethod
    def safe_id(value: object) -> bool:
        return isinstance(value, str) and len(value) == 32 and all(ch in "0123456789abcdef" for ch in value)

    def save(self) -> None:
        # Keep the same camelCase index format as the Windows application.
        atomic_json(self.index, {"vms": list(self.machines.values())})
        for machine in self.machines.values():
            atomic_json(self.home / "machines" / machine["id"] / "machine.json", machine)

    def create(self, name: object, size: object = "m") -> dict:
        if not isinstance(name, str) or not name.strip():
            raise SoilError("A machine name is required.")
        if not isinstance(size, str) or size.lower() not in SIZES:
            raise SoilError("Unknown machine size.")
        size = size.lower()
        ram, disk, cpus = SIZES[size]
        with self.lock:
            name = name.strip()[:40]
            candidate = name
            number = 2
            while any(m.get("name", "").lower() == candidate.lower() for m in self.machines.values()):
                candidate = name + " " + str(number)
                number += 1
            mid = uuid.uuid4().hex
            machine = {
                "id": mid, "name": candidate, "ramMb": ram, "cpu": cpus,
                "diskGb": disk, "size": size, "os": "Soil Desktop",
                "kind": "desktop", "createdUtc": utcnow(),
            }
            (self.home / "machines" / mid / "data").mkdir(parents=True, exist_ok=True)
            (self.home / "machines" / mid / "logs").mkdir(parents=True, exist_ok=True)
            self.machines[mid] = machine
            self.save()
            return self._dto(machine)

    def create_native(self, filename: str, content: object, length: int) -> dict:
        """Import a local ELF/AppImage without executing it or starting a session."""
        if (not filename or len(filename) > 128 or
                any(c in filename for c in ("/", "\\", "\x00"))):
            raise SoilError("Supply a simple local filename.")
        if not (1 <= length <= native_apps.MAX_NATIVE_BYTES):
            raise SoilError("Native app must be between 1 byte and 256 MiB.")
        with self.lock:
            mid = uuid.uuid4().hex
            folder = self.home / "apps" / mid
            folder.mkdir(mode=0o700, parents=True)
            suffix = ".AppImage" if filename.lower().endswith(".appimage") else ".elf"
            target = folder / ("app" + suffix)
            try:
                with target.open("xb") as output:
                    remaining = length
                    while remaining:
                        chunk = content.read(min(128 * 1024, remaining))
                        if not chunk:
                            raise SoilError("Native app upload ended early.")
                        output.write(chunk)
                        remaining -= len(chunk)
                native_apps.validate_binary(target)
                target.chmod(0o700)
                name = Path(filename).stem.strip()[:40] or "Linux app"
                machine = {
                    "id": mid, "name": name, "size": "native", "kind": "app",
                    "os": "Linux native (host)", "sourceType": "linux-native",
                    "nativeFile": filename, "nativePath": target.name,
                    "ramMb": 0, "cpu": 0, "diskGb": 0, "createdUtc": utcnow(),
                }
                self.machines[mid] = machine
                self.save()
                return self._dto(machine)
            except Exception:
                self.machines.pop(mid, None)
                shutil.rmtree(folder, ignore_errors=True)
                raise

    def _dto(self, machine: dict) -> dict:
        mid = machine["id"]
        runtime = self.running.get(mid)
        status = "stopped"
        error = None
        uptime = 0
        if runtime:
            primary = runtime.get("app") or runtime.get("qemu")
            if primary.poll() is None:
                status = "running"
                uptime = int(time.monotonic() - runtime["started"])
                if runtime["websockify"].poll() is not None:
                    error = "The browser console disconnected. Check the VM log."
            else:
                error = ("Linux app" if "app" in runtime else "QEMU") + " exited with code " + str(primary.returncode)
                self._cleanup(mid)
        return {
            "id": mid, "name": machine.get("name", "Machine"),
            "status": status, "ramMb": int(machine.get("ramMb") or 1024),
            "cpu": int(machine.get("cpu") or 1),
            "diskGb": int(machine.get("diskGb") or 80),
            "os": machine.get("os", "Soil Desktop"),
            "uptimeSec": uptime, "error": error, "series": [],
            "kind": machine.get("kind", "desktop"),
            "app": ({"status": "ready" if status == "running" else "stopped", "repoUrl": None}
                    if machine.get("sourceType") == "linux-native" else None),
            "agent": None,
        }

    def list(self) -> list[dict]:
        with self.lock:
            return [self._dto(m) for m in self.machines.values()]

    def get(self, mid: str) -> dict | None:
        with self.lock:
            m = self.machines.get(mid)
            return self._dto(m) if m else None

    def image_dir(self) -> Path:
        candidates = [self.home / "images", HERE / "images", HERE.parent / "images"]
        for candidate in candidates:
            if all((candidate / name).is_file() for name in IMAGE_NAMES):
                valid = True
                for name in IMAGE_NAMES:
                    image = candidate / name
                    with image.open("rb") as handle:
                        if handle.read(80).startswith(b"version https://git-lfs.github.com/spec/"):
                            valid = False
                    if image.stat().st_size < 4096:
                        valid = False
                if valid:
                    return candidate.resolve()
        raise SoilError(
            "Usable guest images are missing. Place vmlinuz64, core2.gz, "
            "and soil.squashfs in ~/.soil/images (not Git LFS pointer files)."
        )

    def requirements(self) -> tuple[str, str, str, Path]:
        qemu = shutil.which("qemu-system-x86_64")
        qemu_img = shutil.which("qemu-img")
        ws = shutil.which("websockify")
        novnc = no_vnc_root()
        missing = [name for name, found in (
            ("qemu-system-x86_64", qemu),
            ("qemu-img", qemu_img),
            ("websockify", ws),
            ("noVNC", novnc),
        ) if not found]
        if missing:
            raise SoilError("Install Linux VM dependencies: " + ", ".join(missing))
        return str(qemu), str(qemu_img), str(ws), novnc  # type: ignore[return-value]

    def _cleanup(self, mid: str) -> None:
        runtime = self.running.pop(mid, None)
        if not runtime:
            return
        for name in ("websockify", "vnc", "window_manager", "app", "qemu", "xvfb"):
            process = runtime.get(name)
            if process is None:
                continue
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=3)
        runtime["log"].close()
        runtime["temporary"].cleanup()

    def start(self, mid: str) -> dict | None:
        with self.lock:
            machine = self.machines.get(mid)
            if not machine:
                return None
            if mid in self.running:
                return self._dto(machine)
            if machine.get("sourceType") == "linux-native" and machine.get("kind") == "app":
                appdir = self.home / "apps" / mid
                try:
                    runtime = native_apps.start(
                        appdir / machine["nativePath"], appdir / "home",
                        self.home / "machines" / mid / "logs" / "app.log",
                        machine["nativeFile"])
                except native_apps.NativeAppError as exc:
                    raise SoilError(str(exc)) from exc
                self.running[mid] = runtime
                return self._dto(machine)
            if machine.get("kind", "desktop") != "desktop":
                raise SoilError("This Linux host cannot start the requested machine type.")
            qemu, qemu_img, ws, novnc = self.requirements()
            images = self.image_dir()
            folder = self.home / "machines" / mid
            disk = folder / "data" / "linux-disk.qcow2"
            disk.parent.mkdir(parents=True, exist_ok=True)
            if not disk.exists():
                result = subprocess.run(
                    [qemu_img, "create", "-f", "qcow2", "-F", "raw",
                     "-b", str(images / "soil.squashfs"), str(disk)],
                    capture_output=True, text=True, timeout=30, check=False,
                )
                if result.returncode:
                    raise SoilError("Could not create VM disk: " + result.stderr.strip()[:500])

            # QEMU's VNC display N listens on TCP port 5900 + N.
            display = next((port - 5900 for port in range(5910, 6000)
                            if _port_available(port)), None)
            if display is None:
                raise SoilError("No available localhost VNC port.")
            websocket_port = free_port()
            temporary = tempfile.TemporaryDirectory(prefix="soil-qemu-")
            monitor = Path(temporary.name) / "qmp.sock"
            (folder / "logs").mkdir(parents=True, exist_ok=True)
            log = (folder / "logs" / "vm.log").open("ab", buffering=0)
            kvm = os.access("/dev/kvm", os.R_OK | os.W_OK)
            args = [
                qemu, "-machine", "q35",
                "-accel", "kvm" if kvm else "tcg,thread=multi",
                "-cpu", "host" if kvm else "max",
                "-smp", str(max(1, min(int(machine.get("cpu", 1)), 8))),
                "-m", str(int(machine.get("ramMb") or 1024)),
                "-kernel", str(images / "vmlinuz64"),
                "-initrd", str(images / "core2.gz"),
                "-append", "console=ttyS0 quiet loglevel=3 acpi=off noapic nolapic mitigations=off nozswap",
                "-drive", "file=" + str(disk) + ",format=qcow2,if=virtio",
                "-display", "none", "-vnc", "127.0.0.1:" + str(display),
                "-qmp", "unix:" + str(monitor) + ",server=on,wait=off",
                "-serial", "file:" + str(folder / "logs" / "serial.log"),
                "-monitor", "none", "-no-reboot",
            ]
            qproc = None
            webproc = None
            try:
                qproc = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=log, stderr=log)
                webproc = subprocess.Popen(
                    [ws, "--web", str(novnc), "127.0.0.1:" + str(websocket_port),
                     "127.0.0.1:" + str(5900 + display)],
                    stdin=subprocess.DEVNULL, stdout=log, stderr=log,
                )
                time.sleep(0.2)
                if qproc.poll() is not None:
                    raise SoilError("QEMU failed to launch; inspect " + str(folder / "logs" / "vm.log"))
                if webproc.poll() is not None:
                    raise SoilError("websockify could not start; inspect " + str(folder / "logs" / "vm.log"))
                self.running[mid] = {
                    "qemu": qproc, "websockify": webproc,
                    "websocket_port": websocket_port, "monitor": monitor,
                    "started": time.monotonic(), "temporary": temporary, "log": log,
                }
                return self._dto(machine)
            except Exception:
                for process in (webproc, qproc):
                    if process and process.poll() is None:
                        process.terminate()
                        try:
                            process.wait(timeout=2)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait()
                log.close()
                temporary.cleanup()
                raise

    def console(self, mid: str) -> str | None:
        with self.lock:
            if mid not in self.machines or mid not in self.running:
                return None
            runtime = self.running[mid]
            if (runtime.get("app") or runtime.get("qemu")).poll() is not None:
                self._cleanup(mid)
                return None
            return ("http://127.0.0.1:" + str(runtime["websocket_port"])
                    + "/vnc.html?autoconnect=true&resize=scale&path=websockify")

    def stop(self, mid: str) -> dict | None:
        with self.lock:
            machine = self.machines.get(mid)
            if not machine:
                return None
            runtime = self.running.get(mid)
            if not runtime:
                return self._dto(machine)
            if "qemu" in runtime:
                # Give the guest a chance to shut down cleanly before terminating QEMU.
                qemu = runtime["qemu"]
                try:
                    _qmp_powerdown(runtime["monitor"])
                    qemu.wait(timeout=4)
                except (OSError, TimeoutError, subprocess.TimeoutExpired, ValueError):
                    pass
            self._cleanup(mid)
            return self._dto(machine)

    def delete(self, mid: str) -> bool:
        with self.lock:
            if mid not in self.machines:
                return False
            self.stop(mid)
            del self.machines[mid]
            self.save()
            folder = self.home / "machines" / mid
            if folder.is_dir():
                shutil.rmtree(folder)
            appdir = self.home / "apps" / mid
            if appdir.is_dir():
                shutil.rmtree(appdir)
            return True

    def stop_all(self) -> None:
        with self.lock:
            for mid in list(self.running):
                self.stop(mid)


def _port_available(port: int) -> bool:
    with socket.socket() as sock:
        try:
            sock.bind(("127.0.0.1", port))
            return True
        except OSError:
            return False


def _qmp_powerdown(path: Path) -> None:
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as conn:
        conn.settimeout(1.5)
        conn.connect(str(path))
        conn.recv(4096)  # QMP greeting
        for cmd in ("qmp_capabilities", "system_powerdown"):
            conn.sendall((json.dumps({"execute": cmd}) + "\r\n").encode("ascii"))
            conn.recv(4096)


def install_skills() -> list[dict]:
    home = Path.home()
    xdg = Path(os.getenv("XDG_CONFIG_HOME") or home / ".config")
    candidates = [
        ("Claude Code", Path(os.getenv("CLAUDE_CONFIG_DIR") or home / ".claude")),
        ("Codex", Path(os.getenv("CODEX_HOME") or home / ".codex")),
        ("Sparky", Path(os.getenv("SPARKY_HOME") or home / ".sparky")),
        ("OpenCode", xdg / "opencode"),
    ]
    results = []
    for agent, root in candidates:
        destination = ((home / ".agents" / "skills" / "soil-control" / "SKILL.md")
                       if agent == "Codex" and not os.getenv("CODEX_HOME")
                       else root / "skills" / "soil-control" / "SKILL.md")
        status, message = "not_detected", "No configuration folder was found."
        if root.is_dir():
            try:
                destination.parent.mkdir(parents=True, exist_ok=True)
                if destination.exists():
                    status = "already_installed" if destination.read_bytes() == SKILL_FILE.read_bytes() else "conflict"
                    message = None if status == "already_installed" else "An existing skill was left unchanged."
                else:
                    with destination.open("xb") as output:
                        output.write(SKILL_FILE.read_bytes())
                    status, message = "installed", None
            except OSError as exc:
                status, message = "error", str(exc)
        results.append({"agent": agent, "status": status,
                        "path": str(destination) if status != "not_detected" else None,
                        "message": message})
    return results


class Handler(BaseHTTPRequestHandler):
    server: "SoilServer"

    def log_message(self, fmt: str, *args: object) -> None:
        sys.stderr.write("[soil] " + (fmt % args) + "\n")

    def _allowed(self) -> bool:
        # The app API must never be reachable via DNS rebinding or cross-origin POSTs.
        host = self.headers.get("Host", "")
        if host not in (self.server.origin.removeprefix("http://"),):
            return False
        origin = self.headers.get("Origin")
        return not origin or origin == self.server.origin

    def _send(self, status: int, data: object = None, content_type: str = "application/json") -> None:
        if data is None:
            payload = b""
        elif isinstance(data, bytes):
            payload = data
        else:
            payload = json.dumps(data).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(payload)

    def _json_body(self) -> dict:
        if self.headers.get("Content-Type", "").split(";")[0].strip().lower() != "application/json":
            raise SoilError("Expected application/json.")
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if length < 1 or length > 1024 * 1024:
                raise SoilError("Invalid request length.")
            body = json.loads(self.rfile.read(length))
            if not isinstance(body, dict):
                raise ValueError("Expected an object")
            return body
        except (ValueError, UnicodeError) as exc:
            raise SoilError("Invalid JSON request.") from exc

    def _dispatch(self) -> None:
        if not self._allowed():
            self._send(403, {"error": "Access denied."})
            return
        path = unquote(urlsplit(self.path).path)
        host = self.server.host
        try:
            if self.command in ("GET", "HEAD"):
                if path == "/api/health":
                    return self._send(200, {"ok": True})
                if path == "/api/info":
                    return self._send(200, {"name": "Soil", "version": "4.2.1-linux",
                                            "home": str(host.home), "schema": 1})
                if path == "/api/ui":
                    state = self.server.ui_state.copy()
                    state["onboardingComplete"] = True  # Linux has no Windows onboarding flow.
                    return self._send(200, state)
                if path == "/api/vms":
                    return self._send(200, host.list())
                if path == "/api/skill":
                    if not SKILL_FILE.is_file():
                        return self._send(404, {"error": "Skill not bundled."})
                    return self._send(200, SKILL_FILE.read_bytes(), "text/markdown; charset=utf-8")
                if path == "/api/github/status":
                    return self._send(200, {"configured": False, "connected": False, "login": None,
                                            "avatarUrl": None, "workerUrl": "", "connecting": False,
                                            "error": "GitHub integration is not yet supported on Linux."})
                if path == "/api/github/repos":
                    return self._send(501, {"error": "GitHub imports are not yet supported on Linux."})
                parts = path.strip("/").split("/")
                if len(parts) == 4 and parts[:2] == ["api", "vms"]:
                    mid, action = parts[2:]
                    if not host.safe_id(mid) or not host.get(mid):
                        return self._send(404, {"error": "Machine not found."})
                    if action == "console":
                        console_url = host.console(mid)
                        return self._send(200, {"url": console_url}) if console_url else self._send(409, {"error": "Machine not running."})
                    if action in ("frame.jpg", "stream.mjpg"):
                        return self._send(404, {"error": "Linux uses the noVNC console."})
                return self._static(path)
            if self.command == "POST":
                if path == "/api/apps/native":
                    if self.headers.get("Content-Type", "").split(";")[0].lower() != "application/octet-stream":
                        return self._send(415, {"error": "Expected application/octet-stream."})
                    try:
                        length = int(self.headers.get("Content-Length", "0"))
                    except ValueError:
                        return self._send(400, {"error": "Invalid request length."})
                    if length < 1 or length > native_apps.MAX_NATIVE_BYTES:
                        return self._send(413, {"error": "Linux native app limit is 256 MiB."})
                    filename = unquote(self.headers.get("X-Soil-Filename", ""))
                    return self._send(200, host.create_native(filename, self.rfile, length))
                body = self._json_body()
                if path == "/api/ui":
                    self.server.ui_state.update({k: body[k] for k in ("purpose", "agents", "package", "theme")
                                                 if k in body})
                    atomic_json(host.home / "ui-state.json", self.server.ui_state)
                    return self._send(200, dict(self.server.ui_state, onboardingComplete=True))
                if path == "/api/vms":
                    return self._send(200, host.create(body.get("name"), body.get("size") or "m"))
                if path == "/api/skill/install":
                    return self._send(200, install_skills())
                if path in ("/api/agents", "/api/apps"):
                    return self._send(501, {"error": "The Linux preview supports desktop VMs only."})
                parts = path.strip("/").split("/")
                if len(parts) == 4 and parts[:2] == ["api", "vms"] and host.safe_id(parts[2]):
                    mid, action = parts[2:]
                    if action == "start":
                        dto = host.start(mid)
                        return self._send(200, dto) if dto else self._send(404, {"error": "Machine not found."})
                    if action == "stop":
                        dto = host.stop(mid)
                        return self._send(200, dto) if dto else self._send(404, {"error": "Machine not found."})
                    if action == "input":
                        # Browser-side noVNC handles pointer and keyboard input.
                        return self._send(204) if host.get(mid) else self._send(404, {"error": "Machine not found."})
                return self._send(404, {"error": "Unknown endpoint."})
            if self.command == "DELETE":
                parts = path.strip("/").split("/")
                if len(parts) == 3 and parts[:2] == ["api", "vms"] and host.safe_id(parts[2]):
                    if host.delete(parts[2]):
                        return self._send(200, {"id": parts[2], "deleted": True})
                return self._send(404, {"error": "Machine not found."})
            return self._send(405, {"error": "Method not allowed."})
        except (SoilError, native_apps.NativeAppError) as exc:
            return self._send(400, {"error": str(exc)})
        except (OSError, subprocess.SubprocessError) as exc:
            self.log_message("error: %s", exc)
            return self._send(500, {"error": str(exc)})

    def _static(self, request_path: str) -> None:
        if request_path == "/linux-ui.js":
            source = HERE / "linux-ui.js"
        else:
            relative = request_path.lstrip("/") or "app.html"
            if relative == "index.html":
                relative = "app.html"
            source = (UI_DIR / relative).resolve()
            if not source.is_relative_to(UI_DIR.resolve()):
                return self._send(404, {"error": "Not found."})
        if not source.is_file():
            return self._send(404, {"error": "Not found."})
        data = source.read_bytes()
        if source.name == "app.html":
            data = data.replace(b"</body>", b'<script src="/linux-ui.js"></script>\n</body>')
        content_type = mimetypes.guess_type(source.name)[0] or "application/octet-stream"
        if content_type in ("text/html", "text/css", "application/javascript", "text/javascript"):
            content_type += "; charset=utf-8"
        return self._send(200, data, content_type)

    def do_GET(self) -> None:
        self._dispatch()

    def do_HEAD(self) -> None:
        self._dispatch()

    def do_POST(self) -> None:
        self._dispatch()

    def do_DELETE(self) -> None:
        self._dispatch()


class SoilServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, host: LinuxHost):
        super().__init__(("127.0.0.1", 0), Handler)
        self.host = host
        self.origin = "http://127.0.0.1:" + str(self.server_port)
        path = host.home / "ui-state.json"
        self.ui_state = {"onboardingComplete": True, "purpose": "testing",
                         "agents": ["codex", "claude", "sparky"], "package": "tiny",
                         "theme": "light"}
        try:
            if path.exists():
                self.ui_state.update(json.loads(path.read_text(encoding="utf-8")))
        except (ValueError, OSError):
            pass


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Soil for Linux")
    parser.add_argument("--home", type=Path, default=Path(os.getenv("SOIL_HOME", "~/.soil")))
    parser.add_argument("--no-browser", action="store_true", help="Do not open a web browser")
    parser.add_argument("--check", action="store_true", help="Check QEMU and guest image requirements")
    args = parser.parse_args(argv)
    if sys.platform != "linux":
        parser.error("This launcher is for Linux only.")
    try:
        host = LinuxHost(args.home)
    except SoilError as exc:
        print("Soil: " + str(exc), file=sys.stderr)
        return 1
    if args.check:
        try:
            host.requirements()
            print("Guest images: " + str(host.image_dir()))
            print("Linux dependencies OK; KVM: " +
                  ("available" if os.access("/dev/kvm", os.R_OK | os.W_OK) else "TCG fallback"))
            return 0
        except SoilError as exc:
            print("Soil: " + str(exc), file=sys.stderr)
            return 1
    if not UI_DIR.joinpath("app.html").is_file():
        print("Soil: app.html is missing from the installation.", file=sys.stderr)
        return 1
    server = SoilServer(host)
    def terminate(_signum: int, _frame: object) -> None:
        # Let serve_forever exit cleanly rather than deadlocking in its thread.
        threading.Thread(target=server.shutdown, daemon=True).start()
    signal.signal(signal.SIGTERM, terminate)
    signal.signal(signal.SIGINT, terminate)
    print("Soil for Linux: " + server.origin, flush=True)
    print("VMs are only started when you press Start. Ctrl+C stops all running VMs.", flush=True)
    if not args.no_browser:
        webbrowser.open(server.origin)
    try:
        server.serve_forever(poll_interval=0.2)
    finally:
        server.server_close()
        host.stop_all()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
