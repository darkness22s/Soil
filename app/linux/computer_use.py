"""On-demand VM computer-use adapter powered by the MIT-licensed vncdotool CLI.

No desktop automation is implemented here: the published vncdotool vncdo
command handles the VNC/RFB protocol, screenshots, keyboard and pointer.
Each call is a short-lived subprocess; no reactor or automation service remains
running between calls, and nothing starts a stopped VM.
"""
from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path


class ComputerUseError(Exception):
    def __init__(self, status: int, message: str):
        super().__init__(message)
        self.status = status


def _coordinate(value: object, name: str) -> str:
    if type(value) is not int or not 0 <= value <= 16383:
        raise ComputerUseError(400, name + " must be an integer between 0 and 16383.")
    return str(value)


def action_command(body: dict) -> list[str]:
    """Validate one agent action and translate it to vncdotool's own commands."""
    if not isinstance(body, dict):
        raise ComputerUseError(400, "Expected a JSON object.")
    action = body.get("action")
    if action in ("click", "double_click", "move", "scroll"):
        xy = ["move", _coordinate(body.get("x"), "x"),
              _coordinate(body.get("y"), "y")]
        if action == "move":
            return xy
        if action == "scroll":
            direction = body.get("direction")
            steps = body.get("steps", 1)
            if direction not in ("up", "down") or type(steps) is not int or not 1 <= steps <= 20:
                raise ComputerUseError(400, "Scroll requires direction up/down and steps 1..20.")
            return xy + ["click", "4" if direction == "up" else "5"] * steps
        button = body.get("button", "left")
        if button not in ("left", "middle", "right"):
            raise ComputerUseError(400, "Button must be left, middle, or right.")
        number = {"left": "1", "middle": "2", "right": "3"}[button]
        if action == "double_click" and button != "left":
            raise ComputerUseError(400, "Double click supports only the left button.")
        return xy + ["click", number] * (2 if action == "double_click" else 1)
    if action == "type":
        content = body.get("text")
        if not isinstance(content, str) or not content or len(content) > 4096:
            raise ComputerUseError(400, "text must be a non-empty string of at most 4096 characters.")
        if "\x00" in content:
            raise ComputerUseError(400, "NUL is not valid keyboard input.")
        return ["type", content]
    if action == "key":
        key = body.get("key")
        allowed = {
            "enter", "return", "tab", "esc", "escape", "backspace", "del", "delete",
            "insert", "home", "end", "prior", "next", "left", "right", "up", "down",
            "space", "shift", "ctrl", "alt", "super", "win",
            "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12",
        }
        if not isinstance(key, str) or not (
            (len(key) == 1 and key.isascii() and key.isprintable()) or
            key in allowed or
            (key.startswith(("ctrl-", "alt-", "shift-")) and
             len(key) <= 24 and all(piece in allowed or
              (len(piece) == 1 and piece.isascii() and piece.isalnum())
              for piece in key.split("-")))
        ):
            raise ComputerUseError(400, "Unsupported key. Use an ASCII key, a named key, or a modifier shortcut.")
        return ["key", key]
    raise ComputerUseError(400, "action must be click, double_click, move, scroll, type, or key.")


def _run(port: int, commands: list[str]) -> None:
    # The bundled vncdotool is invoked as a subprocess: its Twisted event
    # loop terminates with the command instead of occupying an idle thread.
    vendor = Path(__file__).resolve().parent / "vendor"
    env = os.environ.copy()
    if vendor.is_dir():
        env["PYTHONPATH"] = str(vendor) + os.pathsep + env.get("PYTHONPATH", "")
    argv = [sys.executable, "-m", "vncdotool.command", "-s",
            "127.0.0.1::" + str(port), "--timeout", "8", *commands]
    try:
        result = subprocess.run(argv, capture_output=True, timeout=12,
                                env=env, check=False)
    except subprocess.TimeoutExpired as exc:
        raise ComputerUseError(504, "VNC automation timed out.") from exc
    except OSError as exc:
        raise ComputerUseError(503, "The Python computer-use runtime is unavailable.") from exc
    if result.returncode:
        stderr = result.stderr.decode("utf-8", errors="replace")
        if "No module named" in stderr and "vncdotool" in stderr:
            raise ComputerUseError(503, "Bundled vncdotool is missing; reinstall Soil.")
        raise ComputerUseError(502, "VNC automation failed (exit code " +
                               str(result.returncode) + "). Check VM readiness.")


def perform(port: int, body: dict) -> None:
    _run(port, action_command(body))


def screenshot(port: int) -> bytes:
    with tempfile.TemporaryDirectory(prefix="soil-computer-") as folder:
        path = Path(folder) / "screen.png"
        _run(port, ["capture", str(path)])
        try:
            data = path.read_bytes()
        except OSError as exc:
            raise ComputerUseError(502, "VNC did not return a screenshot.") from exc
        if not data.startswith(b"\x89PNG\r\n\x1a\n") or len(data) > 16 * 1024 * 1024:
            raise ComputerUseError(502, "Invalid or oversized VNC screenshot.")
        return data
