#!/usr/bin/env python3
"""soilmk - Soil (.sol) package builder for the Soil Linux distribution.

Self-contained: only needs Python 3.8+. Ed25519 signing/verification uses the
optional `cryptography` package.

Commands:
  new DIR                     scaffold a new app
  pack APPDIR [-o OUT]        build a .sol package
  verify FILE                 check format, digest and signature
  inspect FILE                print header + manifest
  sign FILE --key KEY         (re)sign an existing package
  keygen NAME                 create NAME.key.json + NAME.pub.pem

See ../docs/ for the format and packaging documentation.
"""
import argparse
import base64
import hashlib
import json
import os
import re
import struct
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from mksquashfs import write_squashfs  # noqa: E402

MAGIC = b"S0L1"
FMT_VERSION = 1
HDR_SIZE = 128
DIGEST_OFF = 32
SIG_OFF = 64
KIND_BY_NAME = {"webapp": 1, "installer": 2, "webapp+native": 3, "native": 4}
NAME_BY_KIND = {v: k for k, v in KIND_BY_NAME.items()}
ID_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{1,63}$")
VER_RE = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+$")
PERMS = {"clipboard", "dialogs", "notifications", "openExternal", "exec"}
SKIP_DIRS = {".git", ".hg", ".svn", "node_modules", "dist", "__pycache__"}
MAX_FILE = 256 * 1024 * 1024
MAX_TOTAL = 512 * 1024 * 1024


class Ent:
    __slots__ = ("mode", "uid", "gid", "mtime", "data", "rdev")

    def __init__(self, data, mode):
        self.mode = (0o100000 if not isinstance(mode, int) or mode < 0o100000 else 0) | mode
        if isinstance(mode, int) and mode >= 0o100000:
            self.mode = mode
        self.uid = 0
        self.gid = 0
        self.mtime = 0
        self.data = data
        self.rdev = 0


def _die(msg, code=1):
    sys.stderr.write("soilmk: error: %s\n" % msg)
    raise SystemExit(code)


def _warn(msg):
    sys.stderr.write("soilmk: warning: %s\n" % msg)


def canonical_manifest(m):
    return json.dumps(m, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def safe_rel(p):
    if not isinstance(p, str) or not p:
        return False
    if p.startswith("/") or "\\" in p or ":" in p:
        return False
    return ".." not in p.split("/")


def validate_manifest(m, app_dir):
    errs = []
    if not isinstance(m, dict):
        return ["manifest must be a JSON object"]
    if m.get("schema") != 1:
        errs.append("schema must be 1")
    if not isinstance(m.get("id"), str) or not ID_RE.match(m.get("id", "")):
        errs.append("id must match ^[a-z0-9][a-z0-9._-]{1,63}$")
    if not isinstance(m.get("name"), str) or not (1 <= len(m["name"]) <= 80):
        errs.append("name is required (1..80 chars)")
    if not isinstance(m.get("version"), str) or not VER_RE.match(m.get("version", "")):
        errs.append("version must be MAJOR.MINOR.PATCH")
    kind = m.get("kind")
    if kind not in KIND_BY_NAME:
        errs.append("kind must be one of %s" % ", ".join(sorted(KIND_BY_NAME)))
    perms = m.get("permissions", [])
    if not isinstance(perms, list) or any(p not in PERMS for p in perms):
        errs.append("permissions must be a subset of %s" % sorted(PERMS))
    icon = m.get("icon")
    if not isinstance(icon, str) or not icon.lower().endswith(".png") or not safe_rel(icon):
        errs.append("icon must be a relative .png path")
    elif not os.path.isfile(os.path.join(app_dir, icon)):
        errs.append("icon not found: %s" % icon)
    entry = m.get("entry")
    if kind in ("webapp", "webapp+native", "native"):
        if not isinstance(entry, str) or not safe_rel(entry):
            errs.append("entry must be a relative path")
        elif not os.path.isfile(os.path.join(app_dir, entry)):
            errs.append("entry not found: %s" % entry)
    elif entry is not None and (not isinstance(entry, str) or not safe_rel(entry)):
        errs.append("entry must be a relative path when present")
    execs = m.get("exec", [])
    if not isinstance(execs, list) or any(not isinstance(e, str) or not safe_rel(e) for e in execs):
        errs.append("exec must be an array of relative paths")
    else:
        for e in execs:
            if not os.path.isfile(os.path.join(app_dir, e)):
                errs.append("exec path not found: %s" % e)
    if kind == "webapp+native" and not execs:
        errs.append("webapp+native requires at least one exec entry")
    if kind == "native":
        native_entry = m.get("entry")
        if isinstance(native_entry, str) and safe_rel(native_entry):
            native_file = os.path.join(app_dir, native_entry)
            if os.path.isfile(native_file):
                with open(native_file, "rb") as f:
                    header = f.read(64)
                elf = (len(header) >= 20 and header[:4] == b"\x7fELF" and
                       header[4:6] == b"\x02\x01" and header[18:20] == b"\x3e\x00" and
                       header[16:18] in (b"\x02\x00", b"\x03\x00"))
                script = header.startswith(b"#!")
                if not (elf or script):
                    errs.append("native entry must be an x86_64 Linux ELF, Type 2 AppImage or shebang script")
                if not os.stat(native_file).st_mode & 0o111:
                    errs.append("native entry must be executable (chmod +x)")
        if "terminal" in m and not isinstance(m["terminal"], bool):
            errs.append("terminal must be a boolean")
        if "args" in m and (not isinstance(m["args"], list) or
                            any(not isinstance(a, str) for a in m["args"]) or
                            len(m["args"]) > 32):
            errs.append("args must be an array of at most 32 strings")
    if kind == "installer":
        inst = m.get("install")
        if not isinstance(inst, dict) or not isinstance(inst.get("command"), str):
            errs.append("installer kind requires install.command")
        elif not safe_rel(inst["command"]):
            errs.append("install.command must be a relative path")
        elif not os.path.isfile(os.path.join(app_dir, inst["command"])):
            errs.append("install.command not found: %s" % inst["command"])
        if isinstance(inst, dict) and "args" in inst and not isinstance(inst["args"], list):
            errs.append("install.args must be an array")
    win = m.get("window")
    if win is not None:
        if not isinstance(win, dict):
            errs.append("window must be an object")
        else:
            for k, lo, hi in (("width", 320, 4096), ("height", 240, 4096)):
                v = win.get(k)
                if v is not None and (not isinstance(v, int) or not (lo <= v <= hi)):
                    errs.append("window.%s must be an integer %d..%d" % (k, lo, hi))
            if "resizable" in win and not isinstance(win["resizable"], bool):
                errs.append("window.resizable must be a boolean")
    build = m.get("build")
    if build is not None:
        if not isinstance(build, dict):
            errs.append("build must be an object")
        else:
            command = build.get("command")
            if command is not None and (not isinstance(command, str) or not command.strip()):
                errs.append("build.command must be a non-empty string")
            for key in ("webDir", "entry"):
                value = build.get(key)
                if value is not None and (not isinstance(value, str) or not safe_rel(value)):
                    errs.append("build.%s must be a relative path" % key)
    dev = m.get("dev")
    if dev is not None:
        if not isinstance(dev, dict):
            errs.append("dev must be an object")
        else:
            command = dev.get("command")
            if command is not None and (not isinstance(command, str) or not command.strip()):
                errs.append("dev.command must be a non-empty string")
            url = dev.get("url")
            if url is not None and not isinstance(url, str):
                errs.append("dev.url must be a string")
    return errs


def collect_tree(app_dir):
    vfs = {}
    total = 0
    for root, dirs, files in os.walk(app_dir, followlinks=False):
        rel_root = os.path.relpath(root, app_dir)
        rel_root = "" if rel_root == "." else rel_root.replace(os.sep, "/")
        dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS)
        for name in sorted(files):
            full = os.path.join(root, name)
            rel = (rel_root + "/" + name) if rel_root else name
            if rel_root == "" and (name == "soil.json" or name.lower().endswith(".sol")):
                continue
            if os.path.islink(full):
                _die("symlink rejected: %s (v1 payloads must be plain files)" % rel)
            st = os.stat(full)
            if not os.path.isfile(full):
                _die("not a regular file: %s" % rel)
            if st.st_size > MAX_FILE:
                _die("file too large (>%d MiB): %s" % (MAX_FILE // 1048576, rel))
            with open(full, "rb") as f:
                data = f.read()
            total += len(data)
            if total > MAX_TOTAL:
                _die("payload too large (>%d MiB)" % (MAX_TOTAL // 1048576))
            mode = 0o755 if (st.st_mode & 0o111) else 0o644
            vfs[rel] = Ent(data, mode)
        for d in dirs:
            full = os.path.join(root, d)
            if os.path.islink(full):
                _die("symlink rejected: %s" % os.path.relpath(full, app_dir))
    return vfs, total


def read_manifest(app_dir):
    path = os.path.join(app_dir, "soil.json")
    if not os.path.isfile(path):
        _die("soil.json not found in %s" % app_dir)
    try:
        with open(path, "rb") as f:
            raw = f.read()
        m = json.loads(raw.decode("utf-8"))
    except Exception as e:
        _die("invalid soil.json: %s" % e)
    errs = validate_manifest(m, app_dir)
    if errs:
        for e in errs:
            sys.stderr.write("soilmk: manifest: %s\n" % e)
        raise SystemExit(1)
    return m


def default_out_name(m):
    base = re.sub(r"[^A-Za-z0-9._-]+", "", m["name"]) or m["id"]
    return base + ".sol"


def build_payload(vfs, out_path, compressor):
    prefixes = sorted({p.split("/")[0] for p in vfs})
    write_squashfs(vfs, prefixes, out_path, compressor=compressor,
                   jobs=max(1, min(8, os.cpu_count() or 4)))


def pack(app_dir, out, sign_key=None, compressor="gzip"):
    m = read_manifest(app_dir)
    vfs, total = collect_tree(app_dir)
    mb_raw = canonical_manifest(m)
    vfs["manifest.json"] = Ent(mb_raw, 0o644)
    # pad the on-disk manifest region so payload starts on a 512-byte sector
    # boundary (loop devices require sector-aligned offsets)
    pad = (-(HDR_SIZE + len(mb_raw))) % 512
    mb = mb_raw + b" " * pad

    fd, tmp = tempfile.mkstemp(prefix="soilmk-", suffix=".sqfs")
    os.close(fd)
    try:
        build_payload(vfs, tmp, compressor)
        psize = os.path.getsize(tmp)
        with open(tmp, "rb") as f:
            payload_digest = hashlib.sha256()
            # digest covers manifest || payload; hash manifest first
            payload_digest.update(mb)
            while True:
                chunk = f.read(1 << 20)
                if not chunk:
                    break
                payload_digest.update(chunk)
        digest = payload_digest.digest()
        kind = KIND_BY_NAME[m["kind"]]
        header = struct.pack("<4sHHIIQQ", MAGIC, FMT_VERSION, kind,
                             HDR_SIZE + len(mb), len(mb), HDR_SIZE + len(mb), psize)
        header += digest + b"\x00" * 64
        assert len(header) == HDR_SIZE
        sig = b"\x00" * 64
        if sign_key:
            sig = sign_digest(read_key(sign_key), digest)
        with open(out, "wb") as o:
            o.write(header + mb)
        with open(out, "ab") as o:
            with open(tmp, "rb") as f:
                while True:
                    chunk = f.read(1 << 20)
                    if not chunk:
                        break
                    o.write(chunk)
        if sig != b"\x00" * 64:
            with open(out, "r+b") as f:
                f.seek(SIG_OFF)
                f.write(sig)
    finally:
        try:
            os.unlink(tmp)
        except OSError:
            pass

    fsize = os.path.getsize(out)
    print("packed %s" % out)
    print("  id       %s" % m["id"])
    print("  name     %s %s" % (m["name"], m["version"]))
    print("  kind     %s" % m["kind"])
    print("  files    %d (%.1f KiB raw)" % (len(vfs), total / 1024.0))
    print("  payload  %.1f KiB (squashfs/%s)" % (psize / 1024.0, compressor))
    print("  package  %.1f KiB" % (fsize / 1024.0))
    print("  sha256   %s" % hashlib.sha256(open(out, "rb").read()).hexdigest())
    print("  signing  %s" % ("yes (" + os.path.basename(sign_key) + ")" if sign_key else "no"))
    return 0


def parse_header(path):
    size = os.path.getsize(path)
    if size < HDR_SIZE:
        _die("not a soil package (too short)")
    with open(path, "rb") as f:
        h = f.read(HDR_SIZE)
    magic, ver, kind, hlen, mlen, poff, psize = struct.unpack("<4sHHIIQQ", h[:32])
    if magic != MAGIC:
        _die("not a soil package (bad magic)")
    if ver != FMT_VERSION:
        _die("unsupported format version %d" % ver)
    if kind not in NAME_BY_KIND:
        _die("unknown package kind %d" % kind)
    if hlen != HDR_SIZE + mlen or poff != hlen:
        _die("corrupt header (length fields inconsistent)")
    if poff % 512:
        _die("payload offset not sector-aligned (%d)" % poff)
    if poff + psize != size:
        _die("corrupt package (size mismatch: header says %d bytes, file is %d)"
             % (poff + psize, size))
    if mlen > 1024 * 1024:
        _die("manifest too large")
    return {
        "version": ver, "kind": kind, "kind_name": NAME_BY_KIND[kind],
        "header_len": hlen, "manifest_len": mlen, "payload_offset": poff,
        "payload_size": psize, "digest": h[32:64], "signature": h[64:128],
        "file_size": size,
    }


def load_manifest(path, hdr):
    with open(path, "rb") as f:
        f.seek(HDR_SIZE)
        raw = f.read(hdr["manifest_len"])
    try:
        m = json.loads(raw.decode("utf-8"))
    except Exception as e:
        _die("manifest JSON invalid: %s" % e)
    return m, raw


def compute_digest(path, hdr):
    d = hashlib.sha256()
    with open(path, "rb") as f:
        f.seek(HDR_SIZE)
        raw = f.read(hdr["manifest_len"])
        d.update(raw)
        f.seek(hdr["payload_offset"])
        while True:
            chunk = f.read(1 << 20)
            if not chunk:
                break
            d.update(chunk)
    return d.digest()


def _crypto():
    try:
        from cryptography.hazmat.primitives.asymmetric.ed25519 import (
            Ed25519PrivateKey, Ed25519PublicKey)
        from cryptography.hazmat.primitives import serialization
        return Ed25519PrivateKey, Ed25519PublicKey, serialization
    except ImportError:
        _die("signing requires the 'cryptography' package (pip install cryptography)")


def keygen(name, out_dir="."):
    Ed25519PrivateKey, _, serialization = _crypto()
    key = Ed25519PrivateKey.generate()
    seed = key.private_bytes(serialization.Encoding.Raw,
                             serialization.PrivateFormat.Raw,
                             serialization.NoEncryption())
    pub = key.public_key().public_bytes(serialization.Encoding.Raw,
                                        serialization.PublicFormat.Raw)
    os.makedirs(out_dir, exist_ok=True)
    key_path = os.path.join(out_dir, name + ".key.json")
    pem_path = os.path.join(out_dir, name + ".pub.pem")
    with open(key_path, "w", encoding="utf-8") as f:
        json.dump({
            "type": "soil-ed25519-v1",
            "comment": "PRIVATE KEY - never distribute",
            "seed": base64.b64encode(seed).decode(),
            "public": base64.b64encode(pub).decode(),
        }, f, indent=2)
    try:
        os.chmod(key_path, 0o600)
    except OSError:
        pass
    pem = key.public_key().public_bytes(serialization.Encoding.PEM,
                                        serialization.PublicFormat.SubjectPublicKeyInfo)
    with open(pem_path, "wb") as f:
        f.write(pem)
    fp = hashlib.sha256(pub).hexdigest()[:16]
    print("keygen: %s (private), %s (public)" % (key_path, pem_path))
    print("fingerprint: %s" % fp)


def read_key(path):
    Ed25519PrivateKey, _, serialization = _crypto()
    with open(path, encoding="utf-8") as f:
        k = json.load(f)
    if k.get("type") != "soil-ed25519-v1":
        _die("unsupported key type in %s" % path)
    seed = base64.b64decode(k["seed"])
    if len(seed) != 32:
        _die("bad key seed in %s" % path)
    return Ed25519PrivateKey.from_private_bytes(seed)


def sign_digest(key, digest):
    return key.sign(digest)


def verify_signature(digest, sig, pub_pem_paths):
    Ed25519PrivateKey, Ed25519PublicKey, serialization = _crypto()
    results = []
    for p in pub_pem_paths:
        try:
            with open(p, "rb") as f:
                pub = serialization.load_pem_public_key(f.read())
            pub.verify(sig, digest)
            results.append((p, True, None))
        except Exception as e:
            results.append((p, False, str(e)))
    return results


def cmd_verify(args):
    hdr = parse_header(args.file)
    m, _ = load_manifest(args.file, hdr)
    digest = compute_digest(args.file, hdr)
    ok_digest = digest == hdr["digest"]
    print("package:  %s" % args.file)
    print("id:       %s %s (%s)" % (m.get("id"), m.get("version"), hdr["kind_name"]))
    print("size:     %.1f KiB (payload %.1f KiB)" % (
        hdr["file_size"] / 1024.0, hdr["payload_size"] / 1024.0))
    print("digest:   %s %s" % (digest.hex(), "OK" if ok_digest else "MISMATCH"))
    if not ok_digest:
        _die("integrity check failed", 2)
    signed = hdr["signature"] != b"\x00" * 64
    if not signed:
        print("signature: none (unsigned package)")
        return 0 if not args.require_signature else _die_code("unsigned", 3)
    keys = list(args.key or [])
    if args.trust:
        keys.extend(sorted(os.path.join(args.trust, n) for n in os.listdir(args.trust)
                           if n.endswith(".pem")))
    if not keys:
        print("signature: present (no verification key given; use --key or --trust)")
        return 0
    results = verify_signature(digest, hdr["signature"], keys)
    good = [p for p, ok, _ in results if ok]
    for p, ok, err in results:
        print("  %-40s %s" % (os.path.basename(p), "valid" if ok else "invalid"))
    if good:
        print("signature: VALID (%d trusted key match)" % len(good))
        return 0
    print("signature: INVALID or signed by untrusted key")
    return 3


def _die_code(msg, code):
    sys.stderr.write("soilmk: %s\n" % msg)
    raise SystemExit(code)


def cmd_inspect(args):
    hdr = parse_header(args.file)
    m, _ = load_manifest(args.file, hdr)
    print("magic:          S0L1")
    print("format:         v%d" % hdr["version"])
    print("kind:           %s (%d)" % (hdr["kind_name"], hdr["kind"]))
    print("header_len:     %d" % hdr["header_len"])
    print("manifest_len:   %d" % hdr["manifest_len"])
    print("payload_offset: %d" % hdr["payload_offset"])
    print("payload_size:   %d" % hdr["payload_size"])
    print("digest:         %s" % hdr["digest"].hex())
    print("signature:      %s" % ("present" if hdr["signature"] != b"\x00" * 64 else "none"))
    print("manifest:")
    print(json.dumps(m, indent=2, sort_keys=True))
    return 0


def cmd_sign(args):
    hdr = parse_header(args.file)
    digest = compute_digest(args.file, hdr)
    key = read_key(args.key)
    sig = sign_digest(key, digest)
    with open(args.file, "r+b") as f:
        f.seek(SIG_OFF)
        f.write(sig)
    print("signed %s (ed25519)" % args.file)
    return 0


def solid_png(path, w=128, h=128):
    bg = (237, 231, 220)
    fg = (107, 79, 59)
    rows = b""
    cx, cy, r = w / 2.0, h / 2.0, min(w, h) * 0.36
    for y in range(h):
        row = b"\x00"
        for x in range(w):
            inside = (x - cx) ** 2 + (y - cy) ** 2 <= r * r
            r8, g8, b8 = fg if inside else bg
            row += bytes((r8, g8, b8, 255))
        rows += row

    def chunk(t, d):
        return (struct.pack(">I", len(d)) + t + d +
                struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
           chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


HELLO_HTML = """<!doctype html>
<html lang="en">
<meta charset="utf-8">
<title>__NAME__</title>
<style>
  html,body{height:100%} body{margin:0;font:15px/1.5 system-ui,sans-serif;
  background:#1c1c1e;color:#f2f2f7;display:grid;place-items:center}
  .card{max-width:520px;padding:32px;text-align:center}
  h1{margin:0 0 8px;font-size:28px} p{color:#a1a1a6}
  button{margin:6px;padding:10px 16px;border:0;border-radius:10px;
  background:#0a7cff;color:#fff;font:inherit;cursor:pointer}
</style>
<div class="card">
  <h1>__NAME__</h1>
  <p>This app runs inside the Soil runtime.</p>
  <button id="notify">Notify</button>
  <button id="store">Store value</button>
  <button id="info">Show info</button>
  <pre id="out"></pre>
</div>
<script>
const out = document.getElementById('out');
document.getElementById('notify').onclick = () =>
  soil.notify('Hello from __NAME__', 'The bridge works.');
document.getElementById('store').onclick = async () => {
  await soil.store.set('last', new Date().toISOString());
  out.textContent = 'stored: ' + (await soil.store.get('last')).value;
};
document.getElementById('info').onclick = async () => {
  out.textContent = JSON.stringify(await soil.app.info(), null, 2);
};
</script>
</html>
"""


def cmd_new(args):
    d = args.dir
    if os.path.exists(d) and os.listdir(d):
        _die("directory exists and is not empty: %s" % d)
    os.makedirs(os.path.join(d, "web"), exist_ok=True)
    name = os.path.basename(os.path.abspath(d)).replace("_", " ").title()
    app_id = "org.soil." + re.sub(r"[^a-z0-9]+", "-", os.path.basename(d).lower()).strip("-")
    manifest = {
        "schema": 1, "id": app_id, "name": name, "version": "1.0.0",
        "kind": "webapp", "description": "A new Soil app.",
        "author": "", "entry": "web/index.html", "icon": "icon.png",
        "window": {"width": 900, "height": 640, "resizable": True},
        "permissions": ["notifications"], "exec": [], "install": None,
        "min_ram_mb": 96,
    }
    with open(os.path.join(d, "soil.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    with open(os.path.join(d, "web", "index.html"), "w", encoding="utf-8") as f:
        f.write(HELLO_HTML.replace("__NAME__", name))
    solid_png(os.path.join(d, "icon.png"))
    with open(os.path.join(d, "README.md"), "w", encoding="utf-8") as f:
        f.write("# %s\n\nA Soil web app.\n\n"
                "Build: `python %s pack . -o %s.sol`\n" % (name, os.path.basename(__file__),
                                                            re.sub(r"\s+", "", name)))
    print("scaffolded %s" % d)
    print("next: python %s pack %s -o %s.sol" % (os.path.basename(__file__), d,
                                                 re.sub(r"\s+", "", name)))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(prog="soilmk", description="Soil .sol package tool")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("new", help="scaffold a new app")
    p.add_argument("dir")
    p.set_defaults(func=cmd_new)

    p = sub.add_parser("pack", help="build a .sol package")
    p.add_argument("appdir")
    p.add_argument("-o", "--out")
    p.add_argument("--sign", metavar="KEY")
    p.add_argument("--compress", choices=("gzip", "none"), default="gzip")
    p.set_defaults(func=lambda a: pack(a.appdir, a.out or default_out_name(read_manifest(a.appdir)),
                                       a.sign, a.compress) and 0)

    p = sub.add_parser("verify", help="verify a package")
    p.add_argument("file")
    p.add_argument("--key", action="append", metavar="PUB.pem")
    p.add_argument("--trust", metavar="DIR")
    p.add_argument("--require-signature", action="store_true")
    p.set_defaults(func=cmd_verify)

    p = sub.add_parser("inspect", help="print header and manifest")
    p.add_argument("file")
    p.set_defaults(func=cmd_inspect)

    p = sub.add_parser("sign", help="sign an existing package")
    p.add_argument("file")
    p.add_argument("--key", required=True)
    p.set_defaults(func=cmd_sign)

    p = sub.add_parser("keygen", help="create a signing keypair")
    p.add_argument("name")
    p.add_argument("--out", default=".")
    p.set_defaults(func=lambda a: keygen(a.name, a.out) or 0)

    args = ap.parse_args(argv)
    return args.func(args) or 0


if __name__ == "__main__":
    sys.exit(main())
