#!/usr/bin/env python3
"""soil - a small developer CLI for building Soil desktop apps.

The CLI intentionally stays close to the existing .sol format:

  init     scaffold a vanilla, Vite, or React+TS app
  dev      localhost preview, or a frontend dev server, with the window.soil shim
  build    validate, pack and optionally sign a .sol package
  native   package an existing Linux executable or AppDir
  run      build (if needed) and hand off to the Soil runtime
  verify   inspect a package
  keygen   create an Ed25519 signing key
  doctor   check the local toolchain
"""
import argparse
import json
import mimetypes
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import soilmk  # noqa: E402

TEMPLATE_ROOT = HERE.parent / "templates"
VERSION = "0.1.0"
RELOAD_PATH = "/__soil/events"
BRIDGE_PATH = "/__soil/bridge.js"
DEFAULT_DEV_PORT = 5173
SKIP_WATCH_DIRS = {".git", ".hg", ".svn", "node_modules", "dist", "__pycache__", ".soil-build"}
SKIP_WATCH_FILES = {".DS_Store"}


def die(message, code=1):
    sys.stderr.write("soil: error: %s\n" % message)
    sys.stderr.flush()
    raise SystemExit(code)


def info(message):
    print("soil: %s" % message, flush=True)


def app_dir_from_arg(value):
    path = Path(value).expanduser().resolve()
    if not path.exists():
        die("path does not exist: %s" % value)
    if not path.is_dir():
        die("expected an app directory: %s" % value)
    return path


def load_manifest(app_dir):
    return soilmk.read_manifest(str(app_dir))


def manifest_section(manifest, key):
    value = manifest.get(key)
    if value is None:
        return {}
    if not isinstance(value, dict):
        die("manifest.%s must be an object" % key)
    return value


def resolve_output(app_dir, manifest, requested):
    if requested:
        out = Path(requested).expanduser()
        if not out.is_absolute():
            out = Path.cwd() / out
    else:
        out = app_dir / soilmk.default_out_name(manifest)
    return out


def cmd_init(args):
    target = Path(args.dir).expanduser().resolve()
    if target.is_file():
        die("expected a directory: %s" % target)
    if target.exists() and any(target.iterdir()):
        die("directory exists and is not empty: %s" % target)
    template = TEMPLATE_ROOT / args.template
    if not template.is_dir():
        die("template not found: %s" % template)

    target.mkdir(parents=True, exist_ok=True)
    shutil.copytree(str(template), str(target), dirs_exist_ok=True)

    raw_name = args.name or target.name.replace("-", " ").replace("_", " ").strip()
    name = raw_name.title() or "Soil App"
    slug = re.sub(r"[^a-z0-9]+", "-", raw_name.lower()).strip("-") or "app"
    app_id = "org.soil." + slug[:48]
    if len(app_id) < 2 or not soilmk.ID_RE.match(app_id):
        app_id = "org.soil.app"

    replacements = {
        "__ID__": app_id,
        "__NAME__": name,
        "__SLUG__": slug,
        "__OUT__": re.sub(r"[^A-Za-z0-9._-]+", "", name) + ".sol",
    }
    for path in target.rglob("*"):
        if not path.is_file() or path.name == "icon.png":
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        for old, new in replacements.items():
            text = text.replace(old, new)
        path.write_text(text, encoding="utf-8")

    soilmk.solid_png(str(target / "icon.png"))
    manifest = load_manifest(target)
    if args.template in ("vite", "react-ts"):
        write_dev_bridge(target, manifest)
    info("scaffolded %s" % target)
    info("id: %s" % app_id)
    dev_cmd = 'python "%s" dev "%s"' % (HERE / "soil.py", target)
    if args.template in ("vite", "react-ts"):
        info("next: npm install; %s" % dev_cmd)
    else:
        info("next: %s" % dev_cmd)
    return 0


def stage_web(app_dir, manifest, web_dir, entry):
    source = app_dir / web_dir
    if not source.is_dir():
        die("web output directory not found: %s" % source)
    entry = entry.replace("\\", "/").lstrip("/")
    if not (source / entry).is_file():
        die("entry not found in %s: %s" % (source, entry))
    staging = Path(tempfile.mkdtemp(prefix="soil-build-"))
    try:
        (staging / "web").mkdir()
        shutil.copytree(str(source), str(staging / "web"), dirs_exist_ok=True)
        icon_name = Path(manifest["icon"]).name
        shutil.copy2(str(app_dir / manifest["icon"]), str(staging / icon_name))
        staged = dict(manifest)
        staged["entry"] = "web/" + entry
        staged["icon"] = icon_name
        (staging / "soil.json").write_text(json.dumps(staged, indent=2) + "\n", encoding="utf-8")
        return staging
    except Exception:
        shutil.rmtree(staging, ignore_errors=True)
        raise


def detect_frontend_command(app_dir):
    pkg_path = app_dir / "package.json"
    if not pkg_path.is_file():
        die("--frontend-build auto needs package.json in %s" % app_dir)
    try:
        pkg = json.loads(pkg_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        die("invalid package.json: %s" % exc)
    scripts = pkg.get("scripts")
    if not isinstance(scripts, dict) or not isinstance(scripts.get("build"), str):
        die("package.json has no scripts.build")
    if (app_dir / "pnpm-lock.yaml").is_file():
        return "pnpm run build"
    if (app_dir / "yarn.lock").is_file():
        return "yarn build"
    if (app_dir / "bun.lockb").is_file() or (app_dir / "bun.lock").is_file():
        return "bun run build"
    return "npm run build"


def run_frontend_build(app_dir, command):
    info("frontend build: %s" % command)
    try:
        rc = subprocess.call(command, cwd=str(app_dir), shell=True)
    except OSError as exc:
        die("could not run frontend build: %s" % exc)
    if rc != 0:
        die("frontend build failed (exit %d)" % rc, rc or 1)
    info("frontend build: complete")


def cmd_build(args):
    app_dir = app_dir_from_arg(args.appdir)
    manifest = load_manifest(app_dir)
    build_cfg = manifest_section(manifest, "build")
    out = resolve_output(app_dir, manifest, args.out)
    out.parent.mkdir(parents=True, exist_ok=True)

    frontend_build = getattr(args, "frontend_build", None)
    if frontend_build == "auto":
        frontend_build = detect_frontend_command(app_dir)
    if frontend_build is None:
        frontend_build = build_cfg.get("command")
    web_dir = args.web_dir or build_cfg.get("webDir")
    entry = args.entry or build_cfg.get("entry") or "index.html"

    if frontend_build is not None and not isinstance(frontend_build, str):
        die("build.command must be a string")
    if web_dir is not None and not isinstance(web_dir, str):
        die("build.webDir must be a string")
    if not isinstance(entry, str):
        die("build.entry must be a string")

    if getattr(args, "no_build", False):
        if frontend_build:
            info("frontend build: skipped (--no-build)")
    elif frontend_build:
        run_frontend_build(app_dir, frontend_build)
        if not web_dir and (app_dir / "dist" / "index.html").is_file():
            web_dir = "dist"

    staging = None
    pack_dir = app_dir
    if web_dir:
        staging = stage_web(app_dir, manifest, web_dir, entry)
        pack_dir = staging
    elif (app_dir / "dist" / "index.html").is_file():
        info("note: dist/index.html exists; add build.webDir to soil.json or use --web-dir dist")

    try:
        try:
            soilmk.pack(str(pack_dir), str(out), args.sign, args.compress)
        except SystemExit as exc:
            return int(exc.code or 1)
    finally:
        if staging is not None:
            shutil.rmtree(staging, ignore_errors=True)

    if args.verify:
        try:
            hdr = soilmk.parse_header(str(out))
            digest = soilmk.compute_digest(str(out), hdr)
        except SystemExit as exc:
            return int(exc.code or 1)
        if digest != hdr["digest"]:
            die("verification failed: digest mismatch", 2)
        if hdr["signature"] == b"\x00" * 64:
            info("verified: digest OK (unsigned package)")
        else:
            info("verified: digest OK (signature present)")
    if not args.sign:
        info("tip: sign with --sign app.key.json before distributing")
    info("package: %s" % out)
    return 0


def validate_native_entry(path):
    """Accept runnable Linux x86_64 ELF/AppImage or an interpreter script."""
    if not path.is_file() or path.is_symlink():
        die("native entry must be a regular file, not a symlink: %s" % path)
    with path.open("rb") as file:
        header = file.read(64)
    elf = (len(header) >= 20 and header[:4] == b"\x7fELF" and
           header[4:6] == b"\x02\x01" and header[18:20] == b"\x3e\x00" and
           header[16:18] in (b"\x02\x00", b"\x03\x00"))
    if not elf and not header.startswith(b"#!"):
        die("native entry must be a Linux x86_64 ELF, AppImage or shebang script; "
            ".deb/.rpm/.iso files are not executable apps")
    if path.stat().st_size > soilmk.MAX_FILE:
        die("native entry exceeds the %d MiB per-file limit" %
            (soilmk.MAX_FILE // (1024 * 1024)))


def cmd_native(args):
    """Build a real native-only .sol from a Linux binary or prepared AppDir."""
    source = Path(args.source).expanduser().resolve()
    if not source.exists():
        die("native source does not exist: %s" % source)
    if not source.is_file() and not source.is_dir():
        die("native source must be an ELF/AppImage/script or app directory")
    raw_name = args.name or source.stem if source.is_file() else args.name or source.name
    name = raw_name.strip()
    slug = re.sub(r"[^a-z0-9._-]+", "-", name.lower()).strip("._-")[:48]
    package_id = args.id or ("org.soil." + (slug or "native"))
    version = args.version
    out = Path(args.out).expanduser().absolute() if args.out else (
        Path.cwd() / (re.sub(r"[^A-Za-z0-9._-]", "", name) or "NativeApp")).with_suffix(".sol")
    out.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="soil-native-build-") as tmp:
        staging = Path(tmp)
        app = staging / "app"
        if source.is_dir():
            entry = args.entry or "AppRun"
            if not soilmk.safe_rel(entry):
                die("--entry must be a relative path inside the app directory")
            for root, dirs, files in os.walk(source, followlinks=False):
                for item in dirs + files:
                    if (Path(root) / item).is_symlink():
                        die("native app directory contains a symlink: %s; "
                            "replace it with its in-tree target before packaging" % item)
            shutil.copytree(source, app)
            executable = app / entry
        else:
            if args.entry:
                die("--entry applies only to app directories")
            app.mkdir()
            executable = app / "run"
            shutil.copy2(source, executable)

        validate_native_entry(executable)
        executable.chmod(executable.stat().st_mode | 0o111)
        icon = staging / "icon.png"
        if args.icon:
            src_icon = Path(args.icon).expanduser().resolve()
            if not src_icon.is_file() or src_icon.suffix.lower() != ".png":
                die("--icon must name an existing PNG file")
            shutil.copy2(src_icon, icon)
        else:
            soilmk.solid_png(str(icon))
        manifest = {
            "schema": 1, "id": package_id, "name": name, "version": version,
            "kind": "native", "entry": executable.relative_to(staging).as_posix(),
            "icon": "icon.png", "permissions": [], "args": args.arg,
            "terminal": args.terminal,
        }
        (staging / "soil.json").write_text(json.dumps(manifest, indent=2) + "\n",
                                            encoding="utf-8")
        try:
            soilmk.pack(str(staging), str(out), args.sign, args.compress)
        except SystemExit as exc:
            return int(exc.code or 1)
    header = soilmk.parse_header(str(out))
    if soilmk.compute_digest(str(out), header) != header["digest"]:
        die("native package digest verification failed", 2)
    info("native package verified: %s" % out)
    info("run inside Soil Linux: soil run %s" % out.name)
    return 0


def cmd_run(args):
    target = Path(args.target).expanduser().resolve()
    if target.is_dir():
        app_dir = target
        manifest = load_manifest(app_dir)
        package = resolve_output(app_dir, manifest, args.out)
        build_args = argparse.Namespace(
            appdir=str(app_dir),
            out=str(package),
            sign=args.sign,
            compress=args.compress,
            verify=True,
            web_dir=args.web_dir,
            entry=args.entry,
            frontend_build=args.frontend_build,
            no_build=args.no_build,
        )
        rc = cmd_build(build_args)
        if rc:
            return rc
    elif target.is_file() and target.suffix.lower() == ".sol":
        package = target
    else:
        die("expected an app directory or a .sol package: %s" % target)

    if args.print_only:
        info("package: %s" % package)
        return 0

    soil_bin = args.soil_bin or shutil.which("soil")
    if not soil_bin:
        info("the Soil runtime is not on PATH")
        info("copy %s into Soil and run: soil run %s" % (package.name, package.name))
        return 1

    cmd = [soil_bin, "run"]
    if args.debug:
        cmd.append("--debug")
    cmd.append(str(package))
    env = dict(os.environ)
    if args.debug:
        env["SOIL_DEBUG"] = "1"
    info("running: %s" % " ".join(cmd))
    return subprocess.call(cmd, env=env)


def cmd_verify(args):
    argv = ["verify", args.file]
    for key in args.key or []:
        argv.extend(["--key", key])
    if args.trust:
        argv.extend(["--trust", args.trust])
    if args.require_signature:
        argv.append("--require-signature")
    return soilmk.main(argv)


def cmd_keygen(args):
    soilmk.keygen(args.name, args.out)
    return 0


def cmd_doctor(args):
    import platform

    info("python: %s" % sys.version.split()[0])
    info("platform: %s" % platform.platform())
    info("soilmk: %s" % (HERE / "soilmk.py"))
    try:
        import cryptography  # noqa: F401
        info("cryptography: installed")
    except ImportError:
        info("cryptography: missing (needed only for signing)")
    soil_bin = shutil.which("soil")
    info("soil runtime: %s" % (soil_bin or "not on PATH"))
    info("node: %s" % (shutil.which("node") or "not on PATH"))
    info("npm: %s" % (shutil.which("npm") or "not on PATH"))
    if args.appdir:
        app_dir = app_dir_from_arg(args.appdir)
        manifest = load_manifest(app_dir)
        info("manifest: %s (%s %s)" % (manifest["id"], manifest["name"], manifest["version"]))
        build_cfg = manifest_section(manifest, "build")
        dev_cfg = manifest_section(manifest, "dev")
        if build_cfg.get("command"):
            info("frontend build: %s" % build_cfg["command"])
        if dev_cfg.get("command"):
            info("frontend dev: %s" % dev_cfg["command"])
    return 0


BRIDGE_JS = r"""(function () {
  if (window.soil) return;
  const manifest = __MANIFEST__;
  const prefix = "__soil_dev__:" + manifest.id + ":";

  const store = {
    get(key) {
      return Promise.resolve({value: localStorage.getItem(prefix + String(key))});
    },
    set(key, value) {
      localStorage.setItem(prefix + String(key), String(value));
      return Promise.resolve({});
    },
    delete(key) {
      localStorage.removeItem(prefix + String(key));
      return Promise.resolve({});
    },
    keys() {
      const keys = [];
      for (let i = 0; i < localStorage.length; i++) {
        const key = localStorage.key(i);
        if (key && key.indexOf(prefix) === 0) keys.push(key.slice(prefix.length));
      }
      return Promise.resolve({keys: keys.sort()});
    }
  };

  const api = {
    app: {
      info() {
        return Promise.resolve({
          id: manifest.id,
          name: manifest.name,
          version: manifest.version,
          kind: manifest.kind,
          dataDir: "/tmp/soil-dev/" + manifest.id
        });
      }
    },
    window: {
      setTitle(title) { document.title = String(title); return Promise.resolve({}); },
      close() { window.close(); return Promise.resolve({}); },
      setSize(width, height) { window.resizeTo(Number(width), Number(height)); return Promise.resolve({}); },
      minimize() { return Promise.resolve({}); }
    },
    dialog: {
      openFile(options) {
        return new Promise((resolve) => {
          const input = document.createElement("input");
          input.type = "file";
          const filters = (options && options.filters) || [];
          const accepts = [];
          for (const filter of filters) {
            for (const pattern of filter.patterns || []) {
              if (pattern.indexOf("*.") === 0) accepts.push(pattern.slice(1));
            }
          }
          if (accepts.length) input.accept = accepts.join(",");
          let settled = false;
          const finish = (value) => {
            if (settled) return;
            settled = true;
            input.remove();
            resolve(value);
          };
          input.onchange = () => finish(input.files && input.files[0] ? {path: input.files[0].name} : null);
          window.addEventListener("focus", () => {
            setTimeout(() => {
              if (!input.files || !input.files.length) finish(null);
            }, 250);
          }, {once: true});
          input.click();
        });
      },
      saveFile(options) {
        const name = window.prompt((options && options.title) || "Save as", (options && options.defaultName) || "untitled");
        return Promise.resolve(name === null ? null : {path: name});
      }
    },
    clipboard: {
      async read() {
        if (navigator.clipboard && navigator.clipboard.readText) {
          return {text: await navigator.clipboard.readText()};
        }
        return {text: window.prompt("Clipboard contents") || ""};
      },
      async write(text) {
        if (navigator.clipboard && navigator.clipboard.writeText) {
          await navigator.clipboard.writeText(String(text));
        } else {
          const area = document.createElement("textarea");
          area.value = String(text);
          document.body.appendChild(area);
          area.select();
          document.execCommand("copy");
          area.remove();
        }
        return {};
      }
    },
    shell: {
      openExternal(url) {
        try {
          const parsed = new URL(String(url));
          if (["http:", "https:", "mailto:"].indexOf(parsed.protocol) < 0) {
            return Promise.reject(new Error("openExternal only supports http, https and mailto"));
          }
          window.open(parsed.href, "_blank", "noopener,noreferrer");
          return Promise.resolve({});
        } catch (error) {
          return Promise.reject(error);
        }
      }
    },
    async notify(title, body) {
      if ("Notification" in window) {
        if (Notification.permission === "default") await Notification.requestPermission();
        if (Notification.permission === "granted") new Notification(String(title), {body: String(body)});
      }
      console.info("[soil] notify:", title, body);
      return {};
    },
    store: store,
    exec: {
      run() {
        return Promise.reject(new Error("exec is unavailable in soil dev; use soil run on Soil"));
      }
    }
  };

  Object.defineProperty(window, "soil", {
    value: api,
    configurable: true,
    enumerable: true,
    writable: false
  });
})();
"""


def bridge_script(manifest):
    encoded = json.dumps(manifest, ensure_ascii=False)
    encoded = encoded.replace("&", "\\u0026").replace("<", "\\u003c").replace(">", "\\u003e")
    return BRIDGE_JS.replace("__MANIFEST__", encoded)


def write_dev_bridge(app_dir, manifest):
    dev_dir = app_dir / "dev"
    dev_dir.mkdir(exist_ok=True)
    path = dev_dir / "soil-bridge.js"
    path.write_text(bridge_script(manifest), encoding="utf-8")
    return path


def inject_dev_html(text):
    bridge = '<script src="%s"></script>' % BRIDGE_PATH
    live = """<script>
(() => {
  const events = new EventSource("/__soil/events");
  events.onmessage = (event) => {
    if (event.data === "reload") location.reload();
  };
})();
</script>"""
    lowered = text.lower()
    head = lowered.find("<head")
    if head >= 0:
        end = lowered.find(">", head)
        if end >= 0:
            text = text[:end + 1] + "\n" + bridge + text[end + 1:]
        else:
            text = bridge + "\n" + text
    else:
        text = bridge + "\n" + text
    lowered = text.lower()
    body_end = lowered.rfind("</body>")
    if body_end >= 0:
        text = text[:body_end] + live + "\n" + text[body_end:]
    else:
        text = text + "\n" + live
    return text


class DevState:
    def __init__(self, app_dir, manifest, entry=None):
        self.app_dir = app_dir
        self.manifest = manifest
        self.entry = (entry or manifest["entry"]).replace("\\", "/").lstrip("/")
        self.stop = threading.Event()

    def snapshot(self):
        stamp = []
        for root, dirs, files in os.walk(self.app_dir):
            dirs[:] = sorted(d for d in dirs if d not in SKIP_WATCH_DIRS)
            for name in sorted(files):
                if name in SKIP_WATCH_FILES or name.lower().endswith(".sol"):
                    continue
                path = Path(root) / name
                try:
                    stat = path.stat()
                except OSError:
                    continue
                rel = path.relative_to(self.app_dir).as_posix()
                stamp.append((rel, stat.st_mtime_ns, stat.st_size))
        return tuple(stamp)


class SoilDevHandler(BaseHTTPRequestHandler):
    server_version = "soil-dev/0.1"
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        return

    def send_bytes(self, status, body, content_type):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        state = self.server.soil_state
        parsed = urllib.parse.urlparse(self.path)
        path = urllib.parse.unquote(parsed.path)

        if path == "/":
            location = "/" + urllib.parse.quote(state.entry, safe="/")
            self.send_response(302)
            self.send_header("Location", location)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        if path == BRIDGE_PATH:
            body = bridge_script(state.manifest).encode("utf-8")
            self.send_bytes(200, body, "application/javascript; charset=utf-8")
            return

        if path == RELOAD_PATH:
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "keep-alive")
            self.end_headers()
            previous = state.snapshot()
            try:
                while not state.stop.is_set():
                    time.sleep(0.5)
                    current = state.snapshot()
                    if current != previous:
                        previous = current
                        print("soil: change detected; reloading", flush=True)
                        self.wfile.write(b"data: reload\n\n")
                        self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass
            return

        rel = path.lstrip("/")
        if not rel:
            self.send_error(404)
            return
        try:
            candidate = (state.app_dir / rel).resolve()
            if os.path.commonpath([str(state.app_dir), str(candidate)]) != str(state.app_dir):
                self.send_error(403)
                return
        except (OSError, ValueError):
            self.send_error(403)
            return

        if candidate.is_dir():
            candidate = candidate / "index.html"
        if not candidate.is_file():
            self.send_bytes(404, b"not found", "text/plain; charset=utf-8")
            return

        try:
            data = candidate.read_bytes()
        except OSError as exc:
            self.send_bytes(500, str(exc).encode("utf-8"), "text/plain; charset=utf-8")
            return

        content_type = mimetypes.guess_type(str(candidate))[0] or "application/octet-stream"
        if content_type.startswith("text/html"):
            try:
                data = inject_dev_html(data.decode("utf-8")).encode("utf-8")
            except UnicodeDecodeError:
                pass
            content_type = "text/html; charset=utf-8"
        self.send_bytes(200, data, content_type)


class SoilDevServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def stop_process(proc):
    if proc.poll() is not None:
        return
    if os.name == "nt":
        subprocess.run(
            ["taskkill", "/PID", str(proc.pid), "/T", "/F"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        return
    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
    except OSError:
        proc.terminate()


def wait_for_url(url, proc, timeout=60):
    parsed = urllib.parse.urlsplit(url)
    host = parsed.hostname or "127.0.0.1"
    port = parsed.port or (443 if parsed.scheme == "https" else 80)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            return False
        try:
            with socket.create_connection((host, port), timeout=1):
                return True
        except OSError:
            time.sleep(0.4)
    return False


def run_frontend_dev(app_dir, command, url, no_open):
    info("frontend: %s" % command)
    info("url: %s" % url)
    kwargs = {"cwd": str(app_dir), "shell": True}
    if os.name == "nt":
        kwargs["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
    else:
        kwargs["start_new_session"] = True
    try:
        proc = subprocess.Popen(command, **kwargs)
    except OSError as exc:
        die("could not start frontend: %s" % exc)

    try:
        if not wait_for_url(url, proc):
            if proc.poll() is None:
                info("frontend did not become ready; check its output")
                stop_process(proc)
                return 1
            return int(proc.returncode or 1)
        info("frontend ready; Ctrl+C to stop")
        if not no_open:
            webbrowser.open(url)
        try:
            return int(proc.wait() or 0)
        except KeyboardInterrupt:
            info("stopping")
            return 0
    finally:
        stop_process(proc)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


def cmd_dev(args):
    app_dir = app_dir_from_arg(args.appdir)
    manifest = load_manifest(app_dir)
    if not manifest.get("entry"):
        die("dev requires a package with an entry (webapp or webapp+native)")
    dev_cfg = manifest_section(manifest, "dev")
    frontend_cmd = args.frontend or dev_cfg.get("command")
    if args.frontend and args.web_dir:
        die("--frontend cannot be combined with --web-dir")
    if frontend_cmd and not args.web_dir:
        if not isinstance(frontend_cmd, str):
            die("dev.command must be a string")
        url = args.frontend_url or dev_cfg.get("url") or "http://127.0.0.1:%d/" % args.port
        write_dev_bridge(app_dir, manifest)
        try:
            return run_frontend_dev(app_dir, frontend_cmd, url, args.no_open)
        except KeyboardInterrupt:
            info("stopping")
            return 0
    serve_dir = app_dir / args.web_dir if args.web_dir else app_dir
    if not serve_dir.is_dir():
        die("web output directory not found: %s" % serve_dir)
    entry = args.entry or ("index.html" if args.web_dir else manifest["entry"])
    if not (serve_dir / entry).is_file():
        die("entry not found in %s: %s" % (serve_dir, entry))
    state = DevState(serve_dir, manifest, entry)
    host = args.host
    port = args.port
    httpd = None
    for offset in range(0, 12):
        try:
            httpd = SoilDevServer((host, port + offset), SoilDevHandler)
            port = httpd.server_address[1]
            break
        except OSError:
            if args.port == 0:
                raise
            continue
    if httpd is None:
        die("could not bind a port near %d" % args.port)
    httpd.soil_state = state
    display_host = "127.0.0.1" if host in ("0.0.0.0", "::") else host
    url = "http://%s:%d/" % (display_host, port)
    info("dev server: %s" % url)
    info("entry: %s" % state.entry)
    info("live reload: on; Ctrl+C to stop")
    if not args.no_open:
        threading.Timer(0.35, lambda: webbrowser.open(url)).start()
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        info("stopping")
    finally:
        state.stop.set()
        httpd.server_close()
    return 0


def build_parser():
    ap = argparse.ArgumentParser(prog="soil", description="Build Soil desktop apps")
    ap.add_argument("--version", action="version", version="soil " + VERSION)
    sub = ap.add_subparsers(dest="command", required=True)

    p = sub.add_parser("init", help="scaffold a new app")
    p.add_argument("dir", nargs="?", default=".")
    p.add_argument("--template", default="vanilla", choices=("vanilla", "vite", "react-ts"))
    p.add_argument("--name", help="display name (default: directory name)")
    p.set_defaults(func=cmd_init)

    p = sub.add_parser("dev", help="start a live-reloading local preview")
    p.add_argument("appdir", nargs="?", default=".")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=DEFAULT_DEV_PORT)
    p.add_argument("--no-open", action="store_true", help="do not open a browser")
    p.add_argument("--web-dir", help="serve a built web output directory instead of the app root")
    p.add_argument("--entry", help="entry file relative to --web-dir (default: index.html)")
    p.add_argument(
        "--frontend",
        metavar="CMD",
        help='override the configured frontend dev command, for example: "npm run dev"',
    )
    p.add_argument("--frontend-url", help="URL to wait for with --frontend (default: local port)")
    p.set_defaults(func=cmd_dev)

    p = sub.add_parser("build", help="pack and verify a .sol package")
    p.add_argument("appdir", nargs="?", default=".")
    p.add_argument("-o", "--out")
    p.add_argument("--sign", metavar="KEY", help="Ed25519 private key JSON")
    p.add_argument("--compress", choices=("gzip", "none"), default="gzip")
    p.add_argument("--web-dir", help="stage a built web output directory (for example: dist)")
    p.add_argument("--entry", help="entry file relative to --web-dir (default: index.html)")
    p.add_argument(
        "--frontend-build",
        nargs="?",
        const="auto",
        metavar="CMD",
        help="run a frontend build before packing; omit CMD to auto-detect package.json",
    )
    p.add_argument("--no-build", action="store_true", help="skip the configured frontend build")
    p.add_argument("--no-verify", action="store_false", dest="verify", default=True)
    p.set_defaults(func=cmd_build)

    p = sub.add_parser("native", help="package an existing Linux ELF, AppImage or AppDir")
    p.add_argument("source", help="executable file or prepared app directory")
    p.add_argument("-o", "--out", help="output .sol file")
    p.add_argument("--id", help="stable package ID (default: org.soil.<name>)")
    p.add_argument("--name", help="display name")
    p.add_argument("--version", default="1.0.0")
    p.add_argument("--entry", help="app-directory entry (default: AppRun)")
    p.add_argument("--icon", help="PNG icon (generates a placeholder otherwise)")
    p.add_argument("--arg", action="append", default=[], help="default launch argument (repeatable)")
    p.add_argument("--terminal", action="store_true", help="open in a terminal from desktop launchers")
    p.add_argument("--sign", metavar="KEY", help="Ed25519 private key JSON")
    p.add_argument("--compress", choices=("gzip", "none"), default="gzip")
    p.set_defaults(func=cmd_native)

    p = sub.add_parser("run", help="build if needed and launch in Soil")
    p.add_argument("target", nargs="?", default=".")
    p.add_argument("-o", "--out")
    p.add_argument("--sign", metavar="KEY")
    p.add_argument("--compress", choices=("gzip", "none"), default="gzip")
    p.add_argument("--web-dir", help="stage a built web output directory")
    p.add_argument("--entry", help="entry file relative to --web-dir")
    p.add_argument(
        "--frontend-build",
        nargs="?",
        const="auto",
        metavar="CMD",
        help="run a frontend build before packing; omit CMD to auto-detect package.json",
    )
    p.add_argument("--no-build", action="store_true", help="skip the configured frontend build")
    p.add_argument("--debug", action="store_true")
    p.add_argument("--print-only", action="store_true", help="build and print the package path")
    p.add_argument("--soil-bin", help="path to the Soil runtime binary")
    p.set_defaults(func=cmd_run)

    p = sub.add_parser("verify", help="verify a package")
    p.add_argument("file")
    p.add_argument("--key", action="append", metavar="PUB.pem")
    p.add_argument("--trust", metavar="DIR")
    p.add_argument("--require-signature", action="store_true")
    p.set_defaults(func=cmd_verify)

    p = sub.add_parser("keygen", help="create an Ed25519 signing keypair")
    p.add_argument("name")
    p.add_argument("--out", default=".")
    p.set_defaults(func=cmd_keygen)

    p = sub.add_parser("doctor", help="check the local toolchain")
    p.add_argument("appdir", nargs="?")
    p.set_defaults(func=cmd_doctor)
    return ap


def main(argv=None):
    args = build_parser().parse_args(argv)
    return int(args.func(args) or 0)


if __name__ == "__main__":
    sys.exit(main())
