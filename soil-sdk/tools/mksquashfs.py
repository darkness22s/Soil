"""Minimal squashfs v4 writer (gzip compressor), enough for kernel loop mounts.

Layout written: [superblock][file data blocks][inode table][dir table][id table]
No fragments, no xattrs, no export table. Files store all blocks (incl. final
partial block) in the block list; dir entries use one header per entry
(delta=0) to keep s16 deltas trivially valid.
"""
import zlib
import lzma
import struct
import time
from concurrent.futures import ThreadPoolExecutor

MAGIC = 0x73717368
BLOCK_SIZE = 131072
BLOCK_LOG = 17
META_SIZE = 8192
INVALID = 0xFFFFFFFFFFFFFFFF

T_DIR, T_FILE, T_SYM, T_BLK, T_CHR, T_FIFO, T_SOCK = 1, 2, 3, 4, 5, 6, 7

TYPE_BY_MODE = {0x4000: T_DIR, 0x8000: T_FILE, 0xA000: T_SYM,
                0x6000: T_BLK, 0x2000: T_CHR, 0x1000: T_FIFO, 0xC000: T_SOCK}

_COMPRESS_LEVEL = 6
COMPRESSOR = "gzip"
JOBS = 4
_PARALLEL_MIN = 4096

_XZ_FILTERS = [{"id": lzma.FILTER_LZMA2, "dict_size": BLOCK_SIZE, "lc": 3, "lp": 0, "pb": 2,
                "mode": lzma.MODE_NORMAL, "nice_len": 128, "mf": lzma.MF_BT4, "depth": 0}]


def _compress_payload(raw):
    if COMPRESSOR == "none":
        return raw, True
    if COMPRESSOR == "xz":
        comp = lzma.compress(raw, format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC32,
                             filters=_XZ_FILTERS)
    else:
        comp = zlib.compress(raw, _COMPRESS_LEVEL)
    if len(comp) < len(raw):
        return comp, False
    return raw, True


class MetaStream:
    def __init__(self):
        self.blocks = []
        self.cur = bytearray()
        self._rel = 0

    def point(self):
        if len(self.cur) >= META_SIZE:
            self.flush()
        return (self._rel, len(self.cur))

    def flush(self):
        if not self.cur:
            return
        raw = bytes(self.cur)
        payload, raw_flag = _compress_payload(raw)
        block = struct.pack("<H", len(payload) | (0x8000 if raw_flag else 0)) + payload
        self.blocks.append(block)
        self._rel += len(block)
        self.cur = bytearray()

    def write(self, data):
        data = memoryview(data)
        i = 0
        while i < len(data):
            space = META_SIZE - len(self.cur)
            take = min(space, len(data) - i)
            self.cur += data[i:i + take].tobytes()
            i += take
            if len(self.cur) == META_SIZE:
                self.flush()

    def bytes(self):
        self.flush()
        return b"".join(self.blocks)


class _Node:
    __slots__ = ("name", "ent", "children", "ino")

    def __init__(self, name, ent=None):
        self.name = name
        self.ent = ent
        self.children = {}
        self.ino = 0


def _enc(name):
    return name.encode("utf-8", "surrogateescape")


def _in_prefix(path, prefixes):
    for pre in prefixes:
        if path == pre or path.startswith(pre + "/"):
            return True
    return False


def _build_tree(vfs, prefixes):
    root = _Node("")

    def ensure_dirs(parts, upto):
        node = root
        for part in parts[:upto]:
            child = node.children.get(part)
            if child is None:
                child = _Node(part)
                node.children[part] = child
            node = child
        return node

    for p, ent in vfs.items():
        if not _in_prefix(p, prefixes):
            continue
        parts = p.split("/")
        parent = ensure_dirs(parts, len(parts) - 1)
        name = parts[-1]
        child = parent.children.get(name)
        if child is None:
            child = _Node(name)
            parent.children[name] = child
        child.ent = ent
    return root


def _node_type(node):
    if node.ent is None:
        return T_DIR
    return TYPE_BY_MODE.get(node.ent.mode & 0xF000, T_FILE)


def _precompress_all(root, jobs):
    data_by_key = {}
    stack = [root]
    while stack:
        node = stack.pop()
        ent = node.ent
        if ent is not None and isinstance(ent.data, bytes) and len(ent.data) >= _PARALLEL_MIN:
            data_by_key.setdefault(id(ent.data), ent.data)
        stack.extend(node.children.values())
    tasks = []
    for key, data in data_by_key.items():
        idx = 0
        for i in range(0, len(data), BLOCK_SIZE):
            tasks.append((key, idx, data[i:i + BLOCK_SIZE]))
            idx += 1
    if not tasks:
        return {}

    def work(t):
        return _compress_payload(t[2])

    if jobs > 1 and len(tasks) > 1:
        with ThreadPoolExecutor(max_workers=jobs) as ex:
            results = list(ex.map(work, tasks))
    else:
        results = [work(t) for t in tasks]
    cache = {}
    for (key, idx, _), res in zip(tasks, results):
        cache.setdefault(key, {})[idx] = res
    return cache


def write_squashfs(vfs, prefixes, out_path, compressor="gzip", jobs=4):
    """Write vfs entries under the given path prefixes as a squashfs image.

    Returns (inode_count, entry_count, uncompressed_bytes).
    """
    global COMPRESSOR, JOBS
    COMPRESSOR = compressor
    JOBS = jobs
    root = _build_tree(vfs, prefixes)
    store = _precompress_all(root, jobs)

    uid_map, gid_map = {}, {}
    stack = [root]
    entry_count = 0
    raw_bytes = 0
    while stack:
        node = stack.pop()
        if node.ent is not None:
            uid_map.setdefault(node.ent.uid, len(uid_map))
            gid_map.setdefault(node.ent.gid, len(gid_map))
            if isinstance(node.ent.data, bytes):
                raw_bytes += len(node.ent.data)
            entry_count += 1
        else:
            uid_map.setdefault(0, len(uid_map))
            gid_map.setdefault(0, len(gid_map))
        stack.extend(node.children.values())

    uid_count = len(uid_map)

    def uid_idx(uid):
        return uid_map.get(uid, 0)

    def gid_idx(gid):
        return uid_count + gid_map.get(gid, 0)

    out = open(out_path, "wb")
    out.write(b"\x00" * 96)
    inode_meta = MetaStream()
    dir_meta = MetaStream()

    state = {"ino": 2}

    def put_inode(typ, mode, uid, gid, mtime, ino, payload):
        body = struct.pack("<HHHHII", typ, mode & 0xFFF, uid_idx(uid), gid_idx(gid),
                           mtime & 0xFFFFFFFF, ino) + payload
        if META_SIZE - len(inode_meta.cur) < 64:
            inode_meta.flush()
        ref = inode_meta.point()
        inode_meta.write(body)
        return ref

    def write_data(data):
        start = out.tell()
        sizes = []
        cached = store.get(id(data))
        if cached is None:
            cached = {}
            idx = 0
            for i in range(0, len(data), BLOCK_SIZE):
                cached[idx] = _compress_payload(data[i:i + BLOCK_SIZE])
                idx += 1
        for i in range(len(cached)):
            payload, raw_flag = cached[i]
            out.write(payload)
            sizes.append(len(payload) | (0x1000000 if raw_flag else 0))
        return start, sizes

    def encode_rdev(rdev):
        minor = rdev & 0xFFFFFFFF
        major = (rdev >> 32) & 0xFFFFFFFF
        return (minor & 0xFF) | ((major & 0xFFF) << 8) | (((minor >> 8) & 0xFFF) << 12)

    def serialize_dir(node, ino, parent_ino):
        names = sorted(node.children, key=_enc)
        for n in names:
            node.children[n].ino = state["ino"]
            state["ino"] += 1

        refs = {}
        for n in names:
            child = node.children[n]
            if _node_type(child) == T_DIR:
                refs[n] = serialize_dir(child, child.ino, ino)

        nsub = 0
        for n in names:
            child = node.children[n]
            typ = _node_type(child)
            ent = child.ent
            if typ == T_DIR:
                nsub += 1
                continue
            mode = ent.mode if ent else 0o755
            uid = ent.uid if ent else 0
            gid = ent.gid if ent else 0
            mtime = ent.mtime if ent else 0
            if typ == T_FILE:
                data = ent.data if isinstance(ent.data, bytes) else bytes(ent.data)
                start, sizes = write_data(data)
                payload = struct.pack("<IIII", start, 0xFFFFFFFF, 0, len(data))
                if sizes:
                    payload += struct.pack("<%dI" % len(sizes), *sizes)
            elif typ == T_SYM:
                tgt = ent.data if isinstance(ent.data, bytes) else bytes(ent.data)
                payload = struct.pack("<II", 1, len(tgt)) + tgt
            elif typ in (T_BLK, T_CHR):
                payload = struct.pack("<II", 1, encode_rdev(ent.rdev))
            else:
                payload = struct.pack("<I", 1)
            refs[n] = put_inode(typ, mode, uid, gid, mtime, child.ino, payload)

        listing = bytearray()
        for n in names:
            child = node.children[n]
            nb = _enc(n)
            block_rel, off = refs[n]
            listing += struct.pack("<III", 0, block_rel, child.ino)
            listing += struct.pack("<HhHH", off, 0, _node_type(child), len(nb) - 1)
            listing += nb

        if len(dir_meta.cur) and META_SIZE - len(dir_meta.cur) < 256:
            dir_meta.flush()
        lref = dir_meta.point()
        dir_meta.write(listing)

        if node.ent is not None:
            mode, uid, gid, mtime = node.ent.mode, node.ent.uid, node.ent.gid, node.ent.mtime
        else:
            mode, uid, gid, mtime = 0o40000 | 0o755, 0, 0, 0
        payload = struct.pack("<IIHHI", lref[0], 2 + nsub, len(listing) + 3, lref[1], parent_ino)
        return put_inode(T_DIR, mode, uid, gid, mtime, ino, payload)

    root_ref = serialize_dir(root, 1, 1)
    inode_count = state["ino"] - 1

    inode_table_start = out.tell()
    out.write(inode_meta.bytes())
    directory_table_start = out.tell()
    out.write(dir_meta.bytes())

    id_values = [uid & 0xFFFFFFFF for uid in uid_map]
    id_values += [gid & 0xFFFFFFFF for gid in gid_map]
    chunks = [id_values[i:i + 1024] for i in range(0, len(id_values), 1024)]
    block_offs = []
    for chunk in chunks:
        block_offs.append(out.tell())
        raw = struct.pack("<%dI" % len(chunk), *chunk)
        payload, raw_flag = _compress_payload(raw)
        out.write(struct.pack("<H", len(payload) | (0x8000 if raw_flag else 0)) + payload)
    id_table_start = out.tell()
    for off in block_offs:
        out.write(struct.pack("<Q", off))
    bytes_used = out.tell()

    pad = (4096 - bytes_used % 4096) % 4096
    if pad:
        out.write(b"\x00" * pad)

    sb = struct.pack("<IIIIIHHHHHH",
                     MAGIC, inode_count, int(time.time()) & 0xFFFFFFFF, BLOCK_SIZE,
                     0, 4 if COMPRESSOR == "xz" else 1, BLOCK_LOG, 0, uid_count + len(gid_map), 4, 0)
    sb += struct.pack("<QQQQQQQQ",
                      (root_ref[0] << 16) | root_ref[1], bytes_used, id_table_start,
                      INVALID, inode_table_start, directory_table_start, INVALID, INVALID)
    out.seek(0)
    out.write(sb)
    out.close()
    return inode_count, entry_count, raw_bytes
