"""On-demand REST-to-vncdotool adapter; VNC input and capture stay upstream (MIT)."""
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


# Match vncdotool's published KEYMAP, while accepting familiar agent key names.
ALIASES = {"backspace": "bsp", "escape": "esc", "insert": "ins",
           "prior": "pgup", "next": "pgdn", "alt": "lalt", "win": "super"}
KEYS = set("enter return tab esc bsp delete del ins home end pgup pgdn "
           "left right up down space shift ctrl lalt super".split())
KEYS.update("f" + str(i) for i in range(1, 13))
MODIFIERS = {"shift", "ctrl", "lalt", "super"}


def action_command(body: dict) -> list[str]:
    """Validate a compact action object; return real vncdo commands."""
    if not isinstance(body, dict):
        raise ComputerUseError(400, "Expected a JSON object.")
    action = body.get("action")
    if action in {"move", "click", "double_click", "scroll"}:
        coords = []
        for axis in ("x", "y"):
            value = body.get(axis)
            if type(value) is not int or not 0 <= value <= 16383:
                raise ComputerUseError(400, axis + " must be an integer from 0 to 16383.")
            coords.append(str(value))
        move = ["move", *coords]
        if action == "move":
            return move
        if action == "scroll":
            direction, steps = body.get("direction"), body.get("steps", 1)
            if direction not in ("up", "down") or type(steps) is not int or not 1 <= steps <= 20:
                raise ComputerUseError(400, "Scroll requires up/down direction and 1..20 steps.")
            return move + ["click", "4" if direction == "up" else "5"] * steps
        button = body.get("button", "left")
        if button not in ("left", "middle", "right") or (action == "double_click" and button != "left"):
            raise ComputerUseError(400, "Invalid click button (double-click is left only).")
        return move + ["click", {"left": "1", "middle": "2", "right": "3"}[button]] * (
            2 if action == "double_click" else 1)
    if action == "type":
        value = body.get("text")
        if not isinstance(value, str) or not 0 < len(value) <= 4096 or "\x00" in value:
            raise ComputerUseError(400, "text must be 1..4096 characters without NUL.")
        return ["type", value]
    if action == "key":
        key = body.get("key")
        if isinstance(key, str):
            if len(key) == 1 and key.isascii() and key.isprintable():
                return ["key", key]
            parts = [ALIASES.get(part, part) for part in key.split("-")]
            if (parts[-1] in KEYS and
                    (len(parts) == 1 or all(part in MODIFIERS for part in parts[:-1]))):
                return ["key", "-".join(parts)]
        raise ComputerUseError(400, "Unsupported key or shortcut.")
    raise ComputerUseError(400, "Unknown computer action.")


def _run(port: int, commands: list[str], timeout: int = 12) -> None:
    vendor = Path(__file__).resolve().parent / "vendor"
    env = os.environ.copy()
    if vendor.is_dir():
        env["PYTHONPATH"] = str(vendor) + os.pathsep + env.get("PYTHONPATH", "")
    # One short-lived vncdotool process per request; nothing runs when idle.
    argv = [sys.executable, "-m", "vncdotool.command", "-s",
            "127.0.0.1::" + str(port), "--timeout", str(timeout - 2),
            "--delay", "1", *commands]
    try:
        result = subprocess.run(argv, capture_output=True, timeout=timeout,
                                env=env, check=False)
    except subprocess.TimeoutExpired as exc:
        raise ComputerUseError(504, "VNC automation timed out.") from exc
    except OSError as exc:
        raise ComputerUseError(503, "Python computer-use runtime unavailable.") from exc
    if result.returncode:
        error = result.stderr.decode("utf-8", "replace")
        if "No module named" in error and "vncdotool" in error:
            raise ComputerUseError(503, "Bundled vncdotool missing; reinstall Soil.")
        raise ComputerUseError(502, "VNC automation failed; check guest readiness.")


def perform(port: int, body: dict) -> None:
    command = action_command(body)
    if command[0] != "type":
        return _run(port, command)
    # typefile handles newline/tab reliably and keeps typed text off process argv.
    text = command[1]
    with tempfile.TemporaryDirectory(prefix="soil-type-") as folder:
        path = Path(folder) / "input.txt"
        path.write_text(text, encoding="utf-8")
        _run(port, ["typefile", str(path)], timeout=min(90, 12 + len(text) // 75))


def screenshot(port: int) -> bytes:
    with tempfile.TemporaryDirectory(prefix="soil-screen-") as folder:
        path = Path(folder) / "screen.png"
        _run(port, ["capture", str(path)])
        try:
            data = path.read_bytes()
        except OSError as exc:
            raise ComputerUseError(502, "VNC did not return a screenshot.") from exc
        if len(data) > 16 * 1024 * 1024 or not data.startswith(b"\x89PNG\r\n\x1a\n"):
            raise ComputerUseError(502, "Invalid or oversized VNC screenshot.")
        return data
