"""Real Linux ELF -> Soil HTTP -> isolated display -> VNC -> stop integration.

Opt in with SOIL_NATIVE_E2E=1 on a Linux x86_64 runner with the display
dependencies installed. This does NOT test VM guest isolation.
"""
import base64
import http.client
import json
import os
import platform
import shutil
import socket
import subprocess
import tempfile
import threading
import unittest
from pathlib import Path
from urllib.parse import urlsplit
from urllib.request import urlopen

from app.linux import soil


C_WINDOW = r"""
#include <X11/Xlib.h>
#include <unistd.h>
#include <stdio.h>
int main(void) {
  Display *d = XOpenDisplay(NULL);
  if (!d) { fputs("Cannot open X11 display\n", stderr); return 2; }
  int screen = DefaultScreen(d);
  Window w = XCreateSimpleWindow(d, RootWindow(d, screen), 80, 80,
                                  480, 260, 1, BlackPixel(d, screen),
                                  WhitePixel(d, screen));
  XStoreName(d, w, "Soil native integration smoke");
  XMapWindow(d, w);
  XFlush(d);
  sleep(15);
  XDestroyWindow(d, w);
  XCloseDisplay(d);
  return 0;
}
"""


@unittest.skipUnless(os.getenv("SOIL_NATIVE_E2E") == "1" and
                     platform.system() == "Linux" and
                     platform.machine() in ("x86_64", "AMD64"),
                     "Requires SOIL_NATIVE_E2E=1 on Linux x86_64")
class NativeEndToEnd(unittest.TestCase):
    def setUp(self):
        missing = [command for command in ("gcc", "Xvfb", "x11vnc", "websockify", "xauth")
                   if not shutil.which(command)]
        if missing:
            self.fail("Native app E2E tools missing: " + ", ".join(missing))
        self.temp = tempfile.TemporaryDirectory(prefix="soil-native-e2e-")
        self.addCleanup(self.temp.cleanup)
        temp = Path(self.temp.name)
        src = temp / "hello.c"
        self.binary = temp / "hello.elf"
        src.write_text(C_WINDOW, encoding="utf-8")
        subprocess.run(["gcc", str(src), "-lX11", "-o", str(self.binary)],
                       capture_output=True, check=True)
        self.host = soil.LinuxHost(temp / "home")
        self.addCleanup(self.host.stop_all)
        self.server = soil.SoilServer(self.host)
        worker = threading.Thread(target=self.server.serve_forever, daemon=True)
        worker.start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)

    def request(self, method, path, body=None, headers=None):
        conn = http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=8)
        conn.request(method, path, body=body, headers=headers or {})
        response = conn.getresponse()
        data = response.read()
        status = response.status
        conn.close()
        return status, json.loads(data) if data else None

    def test_real_binary_import_boot_vnc_and_stop(self):
        self.assertEqual(self.host.list(), [])
        data = self.binary.read_bytes()
        status, imported = self.request("POST", "/api/apps/native", data, {
            "Content-Type": "application/octet-stream", "X-Soil-Filename": "hello.elf",
        })
        self.assertEqual(status, 200)
        mid = imported["id"]
        self.assertEqual(imported["status"], "stopped")
        self.assertFalse(self.host.running, "Import must not start background processes")

        status, started = self.request("POST", f"/api/vms/{mid}/start", b"{}", {
            "Content-Type": "application/json",
        })
        self.assertEqual(status, 200)
        self.assertEqual(started["status"], "running")
        self.assertIsNone(self.host.running[mid]["app"].poll())
        status, details = self.request("GET", f"/api/vms/{mid}/console")
        self.assertEqual(status, 200)
        console = urlsplit(details["url"])
        self.assertEqual(console.hostname, "127.0.0.1")
        with urlopen(f"http://127.0.0.1:{console.port}/vnc.html", timeout=5) as page:
            self.assertEqual(page.status, 200)
            self.assertIn(b"noVNC", page.read())

        # Verify the browser-facing websocket can upgrade to a VNC session.
        with socket.create_connection(("127.0.0.1", console.port), timeout=5) as stream:
            stream.settimeout(5)
            key = base64.b64encode(os.urandom(16)).decode("ascii")
            handshake = (
                "GET /websockify HTTP/1.1\r\n"
                f"Host: 127.0.0.1:{console.port}\r\n"
                "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                f"Sec-WebSocket-Key: {key}\r\n"
                "Sec-WebSocket-Version: 13\r\n"
                "Sec-WebSocket-Protocol: binary\r\n\r\n"
            )
            stream.sendall(handshake.encode("ascii"))
            self.assertIn(b"101 Switching Protocols", stream.recv(4096))

        status, stopped = self.request("POST", f"/api/vms/{mid}/stop", b"{}", {
            "Content-Type": "application/json",
        })
        self.assertEqual(status, 200)
        self.assertEqual(stopped["status"], "stopped")
        self.assertEqual(self.host.running, {}, "Stop must release native app processes")
        status, deleted = self.request("DELETE", f"/api/vms/{mid}")
        self.assertEqual(status, 200)
        self.assertTrue(deleted["deleted"])
        self.assertFalse((self.host.home / "apps" / mid).exists())


if __name__ == "__main__":
    unittest.main()
