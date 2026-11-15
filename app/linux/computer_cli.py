#!/usr/bin/env python3
"""Local agent convenience CLI for Soil's authenticated computer-use HTTP API."""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path
from urllib import error, request


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="soil computer")
    parser.add_argument("--home", default=os.getenv("SOIL_HOME", "~/.soil"),
                        help="Soil home directory (default: ~/.soil)")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("list", help="List VMs and statuses")
    capture = commands.add_parser("screenshot", help="Save VM screenshot (PNG)")
    capture.add_argument("vm_id")
    capture.add_argument("output", nargs="?", default="soil-screen.png")
    action = commands.add_parser("action", help="Send one JSON computer action")
    action.add_argument("vm_id")
    action.add_argument("json", help='JSON object, e.g. {"action":"key","key":"enter"}')
    args = parser.parse_args(argv)
    home = Path(args.home).expanduser()

    try:
        endpoint = json.loads((home / "endpoint.json").read_text(encoding="utf-8"))["url"]
        if not isinstance(endpoint, str) or not re.fullmatch(r"http://127\.0\.0\.1:[0-9]{1,5}", endpoint):
            raise ValueError("Invalid local Soil endpoint.")
        path, data, headers = "/api/vms", None, {}
        if args.command != "list":
            if not re.fullmatch(r"[0-9a-f]{32}", args.vm_id):
                raise ValueError("VM ID must be 32 lowercase hexadecimal characters.")
            token = (home / "keys" / "computer-use.token").read_text(encoding="utf-8").strip()
            path = "/api/vms/" + args.vm_id + "/computer/"
            headers["Authorization"] = "Bearer " + token
            if args.command == "screenshot":
                path += "screenshot"
            else:
                path += "action"
                payload = json.loads(args.json)
                if not isinstance(payload, dict):
                    raise ValueError("Action must be a JSON object.")
                data = json.dumps(payload).encode("utf-8")
                headers["Content-Type"] = "application/json"

        # Prevent proxy environment variables from routing capability tokens off-host.
        opener = request.build_opener(request.ProxyHandler({}))
        with opener.open(request.Request(endpoint + path, data=data, headers=headers), timeout=95) as response:
            result = response.read()
        if args.command == "screenshot":
            Path(args.output).write_bytes(result)
            print(args.output)
        else:
            print(result.decode("utf-8"))
        return 0
    except error.HTTPError as exc:
        try:
            message = json.loads(exc.read().decode("utf-8")).get("error", "Request failed.")
        except (UnicodeError, ValueError, AttributeError):
            message = "Request failed."
        print("Soil: HTTP " + str(exc.code) + ": " + message, file=sys.stderr)
    except (OSError, ValueError, KeyError) as exc:
        print("Soil: " + str(exc), file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
