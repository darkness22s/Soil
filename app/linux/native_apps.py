"""On-demand Linux x86_64 native app sessions for Soil's Linux host.

These sessions run as the host user, NOT inside a VM/security sandbox. Only
trusted binaries should be imported. Processes are launched on demand.
"""
from __future__ import annotations

import os
import secrets
import shutil
import socket
import subprocess
import tempfile
import time
from pathlib import Path

MAX_NATIVE_BYTES = 256 * 1024 * 1024


class NativeAppError(Exception):
    pass


def validate_binary(path: Path) -> None:
    """Reject scripts, packages, other architectures and Git LFS pointers."""
    with path.open("rb") as stream:
        header = stream.read(64)
    if len(header) < 20 or header[:4] != b"\x7fELF":
        raise NativeAppError("Only Linux x86_64 ELF executables and AppImages are supported (.deb/.rpm need installation).")
    if header[4] != 2 or header[5] != 1 or header[18:20] != b"\x3e\x00":
        raise NativeAppError("This build only runs little-endian Linux x86_64 executables.")


def requirements() -> tuple[str, str, str, str, Path]:
    xvfb, vnc, ws, xauth = (shutil.which(name) for name in ("Xvfb", "x11vnc", "websockify", "xauth"))
    novnc = None
    for location in (os.getenv("SOIL_NOVNC_DIR"), "/usr/share/novnc", "/usr/share/noVNC", "/usr/local/share/novnc"):
        if location and (Path(location) / "vnc.html").is_file():
            novnc = Path(location).resolve()
            break
    missing = [name for name, path in (("Xvfb", xvfb), ("x11vnc", vnc),
                                       ("websockify", ws), ("xauth", xauth), ("noVNC", novnc)) if not path]
    if missing:
        raise NativeAppError("Install Linux native app display dependencies: " + ", ".join(missing))
    return str(xvfb), str(vnc), str(ws), str(xauth), novnc  # type: ignore[return-value]


def _free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def _wait_socket(port: int, process: subprocess.Popen, timeout: float = 4) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise NativeAppError("A Linux display service exited during startup. Check the app log.")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.15):
                return
        except OSError:
            time.sleep(0.05)
    raise NativeAppError("Timed out waiting for the Linux display service.")


def _wait_display(display: int, process: subprocess.Popen) -> None:
    path = Path(f"/tmp/.X11-unix/X{display}")
    deadline = time.monotonic() + 4
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise NativeAppError("The Xvfb display exited during startup. Check the app log.")
        if path.exists():
            return
        time.sleep(0.05)
    raise NativeAppError("Timed out waiting for the Linux X display.")


def _display_number() -> int:
    for display in range(180, 250):
        if not Path(f"/tmp/.X11-unix/X{display}").exists() and not Path(f"/tmp/.X{display}-lock").exists():
            return display
    raise NativeAppError("There are no free Linux app displays.")


def terminate(process: subprocess.Popen) -> None:
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=2)


def start(binary: Path, workdir: Path, log_path: Path, filename: str) -> dict:
    """Create one ephemeral local graphical session; caller owns its teardown."""
    xvfb, vnc, ws, xauth, novnc = requirements()
    validate_binary(binary)
    display = _display_number()
    rfb_port, ws_port = _free_port(), _free_port()
    while ws_port == rfb_port:
        ws_port = _free_port()
    temporary = tempfile.TemporaryDirectory(prefix="soil-native-")
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log = log_path.open("ab", buffering=0)
    processes: dict[str, subprocess.Popen] = {}
    try:
        auth = Path(temporary.name) / "Xauthority"
        auth.touch(mode=0o600)
        os.chmod(auth, 0o600)
        result = subprocess.run([xauth, "-f", str(auth), "add", f":{display}", ".", secrets.token_hex(16)],
                                capture_output=True, timeout=10, check=False)
        if result.returncode:
            raise NativeAppError("Could not initialize X11 authentication.")
        workdir.mkdir(parents=True, exist_ok=True)
        env = os.environ.copy()
        env.update(DISPLAY=f":{display}", XAUTHORITY=str(auth), HOME=str(workdir),
                   XDG_CONFIG_HOME=str(workdir / ".config"), XDG_CACHE_HOME=str(workdir / ".cache"),
                   XDG_DATA_HOME=str(workdir / ".local" / "share"))
        if filename.lower().endswith(".appimage"):
            # AppImageKit type-2 can extract without FUSE inside constrained hosts.
            env["APPIMAGE_EXTRACT_AND_RUN"] = "1"

        def spawn(key: str, args: list[str]) -> subprocess.Popen:
            process = subprocess.Popen(args, cwd=workdir, env=env, stdin=subprocess.DEVNULL,
                                       stdout=log, stderr=log, start_new_session=True)
            processes[key] = process
            return process

        _wait_display(display, spawn("xvfb", [xvfb, f":{display}", "-screen", "0", "1280x800x24",
                                              "-nolisten", "tcp", "-noreset", "-auth", str(auth)]))
        if shutil.which("openbox"):
            spawn("window_manager", [shutil.which("openbox"), "--sm-disable"])
        _wait_socket(rfb_port, spawn("vnc", [vnc, "-display", f":{display}", "-auth", str(auth),
                                             "-rfbport", str(rfb_port), "-localhost", "-forever",
                                             "-shared", "-quiet", "-noxdamage", "-nopw"]))
        _wait_socket(ws_port, spawn("websockify", [ws, "--web", str(novnc),
                                                   f"127.0.0.1:{ws_port}", f"127.0.0.1:{rfb_port}"]))
        app = spawn("app", [str(binary)])
        time.sleep(0.2)
        if app.poll() is not None:
            raise NativeAppError(f"The Linux app exited during startup (code {app.returncode}). Check the app log.")
        return dict(processes, temporary=temporary, log=log, websocket_port=ws_port,
                    started=time.monotonic())
    except Exception:
        for proc in reversed(list(processes.values())):
            terminate(proc)
        log.close()
        temporary.cleanup()
        raise
