"""Authenticated LAN expert worker; run --help or read docs/MULTI_HOST.md."""
from __future__ import annotations

import argparse
from collections import OrderedDict
import ctypes as C
import hmac
import ipaddress
import math
import mmap
import os
from pathlib import Path
import re
import socket
import struct
import sys
import threading
import time

from gguf_reader import GGUFFile, GGML_TYPES, BLOCK_GEOMETRY

MAGIC, VERSION = 0x5354584E, 1
HEADER = struct.Struct("<IIQII")
H, FF, MAXT, MAX_BODY = 2560, 640, 8, 2 << 20


def all_finite(data):
    """Compatibility path for older worker libraries and protocol-only tests."""
    return all(math.isfinite(v[0]) for v in struct.iter_unpack("<f", data))


def available_memory_bytes():
    """Available physical RAM and Windows commit capacity; no driver or machine settings are changed."""
    if os.name == "nt":
        class MemoryStatus(C.Structure):
            _fields_ = [("length", C.c_uint32), ("load", C.c_uint32)] + [
                (name, C.c_uint64) for name in ("total_phys", "avail_phys", "total_commit", "avail_commit",
                                               "total_virtual", "avail_virtual", "extended_virtual")]
        status = MemoryStatus()
        status.length = C.sizeof(status)
        query = C.WinDLL("kernel32", use_last_error=True).GlobalMemoryStatusEx
        query.argtypes = [C.POINTER(MemoryStatus)]
        query.restype = C.c_int
        if not query(C.byref(status)):
            raise OSError(C.get_last_error(), "cannot query available RAM for expert preload")
        return status.avail_phys, status.avail_commit
    fields = dict((line.split()[0].rstrip(":"), int(line.split()[1]) * 1024)
                  for line in Path("/proc/meminfo").read_text().splitlines() if len(line.split()) >= 2)
    return fields["MemAvailable"], None


def weight_bytes(type_id, n):
    block, size = BLOCK_GEOMETRY[GGML_TYPES[type_id]]
    if n % block:
        raise ValueError("weight row is not aligned to its quantization block")
    return n // block * size


class ExpertFiles:
    """Only mapped files and a bounded cache; never allocate the full expert arena."""
    def __init__(self, pack, native=None, experts=512, cache_mib=512):
        self.experts, self.files, self.maps = experts, {}, {}
        self.cache, self.cache_bytes, self.budget = OrderedDict(), 0, cache_mib << 20
        self.layers = []
        layout = Path(pack) / "native_experts.txt"
        text = layout.read_text(encoding="utf-8")
        match = re.search(r"strata native experts v(\d+)", text)
        if not match or int(match[1]) not in (1, 2, 3, 4):
            raise ValueError("unsupported native_experts.txt version")
        at = 0
        for line in text.splitlines():
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            fields = line.split()
            if len(fields) not in (5, 8, 9):
                raise ValueError("invalid native expert row")
            l, gu, down, offset, blob = map(int, fields[:5])
            if l != len(self.layers) or offset != at:
                raise ValueError("expert table is not contiguous")
            gsize = weight_bytes(gu, H) * FF
            dsize = weight_bytes(down, FF) * H
            if blob != 2 * gsize + dsize:
                raise ValueError("expert blob size differs from format geometry")
            self.layers.append((gu, down, offset, blob, gsize, dsize, fields[5:]))
            at += blob * experts
        if not self.layers or len(self.layers) > 128 or experts != 512:
            raise ValueError("worker requires 1..128 layers with 512 experts")
        self.bin = Path(pack) / "experts.bin"
        self.roles = []
        if self.bin.exists():
            if self.bin.stat().st_size != at:
                raise ValueError("experts.bin has the wrong size")
            self._map(self.bin)
        else:
            if not native:
                raise ValueError("--native is required when experts.bin is absent")
            native = Path(native).resolve()
            headers = {}
            for l, (_, _, _, _, gsize, dsize, fields) in enumerate(self.layers):
                if len(fields) < 3:
                    raise ValueError("layout lacks GGUF offsets")
                offsets = list(map(int, fields[:3]))
                names = fields[3].split(",") if len(fields) == 4 else [""]
                if len(names) == 1:
                    names *= 3
                if len(names) != 3:
                    raise ValueError("invalid per-role shard column")
                roles = []
                for r, (name, off) in enumerate(zip(names, offsets)):
                    path = native.with_name(name) if name else native
                    if name and (Path(name).name != name or name in (".", "..")):
                        raise ValueError("shard column must be a filename")
                    if path not in headers:
                        headers[path] = GGUFFile(path)
                    header = headers[path]
                    wanted = f"blk.{l}.ffn_{('gate', 'up', 'down')[r]}_exps.weight"
                    tensor = next((t for t in header.tensors if t.name == wanted), None)
                    ty = self.layers[l][0 if r < 2 else 1]
                    shape = [H, FF, experts] if r < 2 else [FF, H, experts]
                    if (tensor is None or tensor.type_id != ty or tensor.shape != shape
                            or header.data_start + tensor.offset != off):
                        raise ValueError(f"{wanted}: layout does not match GGUF")
                    size = gsize if r < 2 else dsize
                    mm = self._map(path)
                    if off < 0 or off + size * experts > len(mm):
                        raise ValueError(f"{wanted}: tensor outside file")
                    roles.append((path, off, size))
                self.roles.append(roles)

    def _map(self, path):
        if path not in self.maps:
            f = open(path, "rb")
            self.files[path] = f
            self.maps[path] = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        return self.maps[path]

    def blob(self, layer, expert):
        _, _, off, size, _, _, _ = self.layers[layer]
        if self.roles:
            return b"".join(self.maps[path][base + expert * stride:base + (expert + 1) * stride]
                            for path, base, stride in self.roles[layer])
        return self.maps[self.bin][off + expert * size:off + (expert + 1) * size]

    def pointer(self, layer, expert):
        key = (layer, expert)
        if key in self.cache:
            self.cache.move_to_end(key)
            return self.cache[key]
        blob = self.blob(layer, expert)
        if len(blob) > self.budget:
            raise ValueError("RAM cache is smaller than one expert")
        while self.cache_bytes + len(blob) > self.budget:
            _, old = self.cache.popitem(last=False)
            self.cache_bytes -= C.sizeof(old)
        buf = C.create_string_buffer(blob, len(blob))
        self.cache[key] = buf
        self.cache_bytes += len(blob)
        return buf

    def preload(self, index, count, assignment=None, progress=print, headroom_bytes=8 << 30):
        """Materialize this worker's whole assignment in the same cache used by compute()."""
        if not 1 <= count <= 16 or not 0 <= index < count:
            raise ValueError("invalid preload worker assignment")
        if assignment is not None and (len(assignment) != self.experts or
                any(type(owner) is not int or not 0 <= owner < count for owner in assignment)):
            raise ValueError("invalid preload expert owner table")
        owned = [e for e in range(self.experts)
                 if (assignment[e] if assignment is not None else e % count) == index]
        if not owned:
            raise ValueError("preload assignment contains no experts")
        total = sum(layer[3] * len(owned) for layer in self.layers)
        if total > self.budget:
            raise ValueError(f"expert preload needs {total / 2**30:.2f} GiB but RAM cache budget is "
                             f"{self.budget / 2**30:.2f} GiB; increase ram_cache_mib or disable preload_experts")
        resident = sum(C.sizeof(self.cache[(l, e)]) for l in range(len(self.layers)) for e in owned
                       if (l, e) in self.cache)
        needed = total - resident
        if needed:
            physical, commit = available_memory_bytes()
            if physical < needed + headroom_bytes or (commit is not None and commit < needed + headroom_bytes):
                raise ValueError(f"expert preload needs {needed / 2**30:.2f} GiB more plus "
                                 f"{headroom_bytes / 2**30:.1f} GiB headroom; available RAM "
                                 f"{physical / 2**30:.2f} GiB, commit "
                                 f"{commit / 2**30 if commit is not None else float('inf'):.2f} GiB; "
                                 "close other processes, reduce the assignment, or disable preload_experts")
        started = time.perf_counter()
        pairs = len(self.layers) * len(owned)
        progress(f"preload: reading {pairs} experts ({total / 2**30:.2f} GiB) into RAM before listening")
        completed = 0
        try:
            for l in range(len(self.layers)):
                for e in owned:
                    self.pointer(l, e)
                    completed += 1
                progress(f"preload: layer {l + 1}/{len(self.layers)}, {completed}/{pairs} experts, "
                         f"cache {self.cache_bytes / 2**30:.2f} GiB, {time.perf_counter() - started:.1f} s")
            if any((l, e) not in self.cache for l in range(len(self.layers)) for e in owned):
                raise RuntimeError("expert preload failed to retain its complete assignment")
        except BaseException:
            # Never leave a partial preload looking ready; release its private buffers before propagating failure.
            self.cache.clear()
            self.cache_bytes = 0
            raise
        elapsed = time.perf_counter() - started
        progress(f"preload complete: {pairs} experts, {total / 2**30:.2f} GiB, {elapsed:.1f} s")
        return pairs, total

    def fingerprint(self):
        value = 14695981039346656037
        def add(data):
            nonlocal value
            for byte in data:
                value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
        add(struct.pack("<IIII", len(self.layers), self.experts, H, FF))
        for l, (gu, down, _, size, _, _, _) in enumerate(self.layers):
            add(struct.pack("<IIQ", gu, down, size))
            for e in (0, self.experts - 1):
                blob = self.blob(l, e)
                add(blob[:4096]); add(blob[-4096:])
        return value

    def close(self):
        self.cache.clear()
        for mm in self.maps.values():
            mm.close()
        for f in self.files.values():
            f.close()


class NativeBackend:
    def __init__(self, library, workers=0, device=-1, gpu_cache_mib=4096):
        # Retain DLL directory handles for dependent HIP/CUDA libraries on Windows.
        self.dll_dirs = []
        library = Path(library).resolve()
        if os.name == "nt":
            for path in [library.parent, library.parent / "rocm" / "bin"]:
                if path.is_dir():
                    self.dll_dirs.append(os.add_dll_directory(str(path)))
        self.lib = C.CDLL(str(library))
        self.lib.strata_network_error.restype = C.c_char_p
        self.lib.strata_network_create.argtypes = [C.c_int, C.c_int, C.c_uint64]
        self.lib.strata_network_create.restype = C.c_void_p
        self.lib.strata_network_destroy.argtypes = [C.c_void_p]
        self.lib.strata_network_format_bytes.argtypes = [C.c_int, C.c_int]
        self.lib.strata_network_format_bytes.restype = C.c_uint64
        self.lib.strata_network_compute.argtypes = [C.c_void_p, C.c_int, C.c_int, C.c_int,
            C.POINTER(C.c_float), C.c_int, C.c_int, C.POINTER(C.c_int32), C.POINTER(C.c_int32),
            C.POINTER(C.c_void_p), C.c_int, C.POINTER(C.c_float)]
        self.lib.strata_network_compute.restype = C.c_int
        # Older deployed libraries remain usable with the original validation.
        self._finite = getattr(self.lib, "strata_network_all_finite", None)
        if self._finite is not None:
            self._finite.argtypes = [C.c_void_p, C.c_uint64]
            self._finite.restype = C.c_int
        self._reduce = getattr(self.lib, "strata_network_reduce", None)
        if self._reduce is not None:
            self._reduce.argtypes = [C.POINTER(C.c_float), C.POINTER(C.c_int32), C.POINTER(C.c_float),
                                     C.c_int, C.c_int, C.c_int, C.POINTER(C.c_float)]
            self._reduce.restype = C.c_int
        self.can_reduce = self._reduce is not None
        self._allocate_buffers()
        self.handle = self.lib.strata_network_create(workers, device, gpu_cache_mib)
        self.failed = False
        if not self.handle:
            raise RuntimeError(self.error())

    def error(self):
        return self.lib.strata_network_error().decode("utf-8", errors="replace")

    def validate(self, source):
        for gu, down, _, size, *_ in source.layers:
            if self.lib.strata_network_format_bytes(gu, down) != size:
                raise ValueError("backend does not support the pack's expert geometry: " + self.error())

    def _allocate_buffers(self):
        # The worker serializes backend calls. Return an immutable snapshot so
        # a later task can reuse these buffers while an earlier result is sent.
        self._rows = (C.c_int32 * (MAXT * 10))()
        self._ids = (C.c_int32 * (MAXT * 10))()
        self._pointers = (C.c_void_p * (MAXT * 10))()
        self._input = (C.c_float * (MAXT * H))()
        self._output = (C.c_float * (MAXT * 10 * H))()
        self._weights = (C.c_float * (MAXT * 10))()
        self._sum = (C.c_float * (MAXT * H))()
        self.last_timings = {}

    def all_finite(self, data):
        if self._finite is None:
            return all_finite(data)
        # c_char_p keeps a reference to bytes without copying. Embedded NULs
        # are fine: the C ABI receives the explicit byte length.
        return bool(self._finite(C.cast(C.c_char_p(data), C.c_void_p), len(data)))

    def compute(self, source, layer, nt, k, entries, activation, weights=None):
        if self.failed:
            raise RuntimeError("native backend failed; restart the worker")
        started = time.perf_counter()
        n = len(entries)
        if not (1 <= nt <= MAXT and 1 <= k <= 10 and 1 <= n <= nt * k
                and len(activation) == nt * H * 4):
            raise ValueError("invalid native task geometry")
        if weights is not None and (not self.can_reduce or len(weights) != n):
            raise ValueError("native backend cannot reduce this task")
        # Hold each buffer even if a subsequent pointer lookup evicts it from the RAM cache.
        buffers = [source.pointer(layer, e) for _, e in entries]
        for i, (row, expert) in enumerate(entries):
            self._rows[i], self._ids[i] = row, expert
            self._pointers[i] = C.addressof(buffers[i])
        C.memmove(self._input, activation, len(activation))
        gu, down = source.layers[layer][:2]
        prepared = time.perf_counter()
        if not self.lib.strata_network_compute(self.handle, gu, down, layer, self._input, nt, n,
                self._rows, self._ids, self._pointers, k, self._output):
            self.failed = True
            raise RuntimeError(self.error())
        computed = time.perf_counter()
        size = n * H * 4
        result_buffer = self._output
        data = None
        if self._finite is not None:
            finite = self._finite(self._output, size)
        else:
            data = C.string_at(self._output, size)
            finite = all_finite(data)
        if not finite:
            self.failed = True
            raise RuntimeError("native backend returned non-finite results")
        checked = time.perf_counter()
        reduced = checked
        if weights is not None:
            for i, weight in enumerate(weights):
                self._weights[i] = weight
            if not self._reduce(self._output, self._rows, self._weights, nt, n, k, self._sum):
                self.failed = True
                raise RuntimeError(self.error())
            result_buffer, size, data = self._sum, nt * H * 4, None
            reduced = time.perf_counter()
        if data is None:
            data = C.string_at(result_buffer, size)
        returned = time.perf_counter()
        self.last_timings = dict(prepare_ms=(prepared - started) * 1000,
                                native_ms=(computed - prepared) * 1000,
                                validate_ms=(checked - computed) * 1000,
                                serialize_ms=(returned - checked) * 1000)
        if weights is not None:
            self.last_timings.update(reduce_ms=(reduced - checked) * 1000,
                                     serialize_ms=(returned - reduced) * 1000)
        return data

    def close(self):
        if self.handle:
            self.lib.strata_network_destroy(self.handle)
            self.handle = None


def receive(conn, size, deadline):
    result = bytearray()
    while len(result) < size:
        left = deadline - time.monotonic()
        if left <= 0:
            raise TimeoutError("frame deadline exceeded")
        conn.settimeout(left)
        data = conn.recv(size - len(result))
        if not data:
            raise EOFError("connection closed")
        result.extend(data)
    return bytes(result)


def frame(conn, timeout):
    deadline = time.monotonic() + timeout
    magic, version, seq, size, kind = HEADER.unpack(receive(conn, HEADER.size, deadline))
    if magic != MAGIC or version != VERSION or size > MAX_BODY:
        raise ValueError("invalid protocol header")
    return seq, kind, receive(conn, size, deadline)


def send(conn, seq, kind, body, timeout):
    conn.settimeout(timeout)
    conn.sendall(HEADER.pack(MAGIC, VERSION, seq, len(body), kind) + body)


def task(body, layers, experts, index, count, assignment=None, finite_check=all_finite, reduced=False):
    if len(body) < 16:
        raise ValueError("truncated task")
    layer, nt, k, n = struct.unpack_from("<IIII", body)
    if not (0 <= layer < layers and 1 <= nt <= MAXT and 1 <= k <= 10 and 1 <= n <= nt * k):
        raise ValueError("invalid task geometry")
    stride = 12 if reduced else 8
    if len(body) != 16 + n * stride + nt * H * 4:
        raise ValueError("invalid task size")
    records = list(struct.iter_unpack("<IIf" if reduced else "<II", body[16:16 + n * stride]))
    entries = [(v[0], v[1]) for v in records]
    weights = [v[2] for v in records] if reduced else None
    if weights is not None and not all(math.isfinite(v) for v in weights):
        raise ValueError("non-finite router weight")
    if (len({r for r, _ in entries}) != n or len({(r // k, e) for r, e in entries}) != n
            or any(r >= nt * k or e >= experts or
                   (assignment[e] if assignment is not None and e < experts else e % count) != index for r, e in entries)):
        raise ValueError("invalid, duplicate, or unowned routing entry")
    activation = body[16 + n * stride:]
    if not finite_check(activation):
        raise ValueError("non-finite activation")
    args = layer, nt, k, entries, activation
    return args + (weights,) if reduced else args


class Worker:
    def __init__(self, source, backend, key, index, count, timeout=60, verbose=True, assignment=None, config_id=None):
        configured = assignment is not None or config_id is not None
        if not 16 <= len(key) <= 256 or not 1 <= count <= (16 if configured else 3) or not 0 <= index < count:
            raise ValueError("invalid key or worker index/count")
        if configured and (not isinstance(assignment, str) or len(assignment) != source.experts
                or any(c not in "0123456789abcdef" or int(c, 16) >= count for c in assignment)
                or not isinstance(config_id, str) or re.fullmatch("[0-9a-f]{64}", config_id) is None):
            raise ValueError("invalid static assignment or configuration digest")
        self.assignment_text, self.config_id = assignment, config_id
        self.assignment = [int(c, 16) for c in assignment] if configured else None
        self.source, self.backend, self.key = source, backend, key
        self.index, self.count, self.timeout = index, count, timeout
        self.fingerprint = source.fingerprint()
        self.lock = threading.Lock()
        self.verbose, self.requests, self.entries = verbose, 0, 0
        self.profile = os.environ.get("STRATA_NETWORK_PROFILE", "0") == "1"
        self.finite_check = getattr(backend, "all_finite", all_finite)

    def session(self, conn):
        seq = 0
        try:
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            seq, kind, hello = frame(conn, self.timeout)
            configured = self.assignment is not None
            key_offset = 28 + 64 + self.source.experts if configured else 28
            reduced = kind == (6 if configured else 5)
            if seq != 0 or kind not in ((3, 6) if configured else (1, 5)) or len(hello) < key_offset:
                raise ValueError("handshake required")
            fingerprint, layers, experts, index, count, hidden = struct.unpack_from("<QIIIII", hello)
            if not hmac.compare_digest(hello[key_offset:], self.key):
                raise ValueError("authentication failed")
            if configured and (hello[28:92] != self.config_id.encode("ascii") or
                               hello[92:key_offset] != self.assignment_text.encode("ascii")):
                raise ValueError("cluster configuration mismatch; restart every node with the same plan")
            if (fingerprint, layers, experts, index, count, hidden) != (
                    self.fingerprint, len(self.source.layers), self.source.experts, self.index, self.count, H):
                raise ValueError("model fingerprint, geometry, or shard assignment mismatch")
            if reduced and not getattr(self.backend, "can_reduce", False):
                raise ValueError("worker library lacks reduced results support; update the backend DLL")
            send(conn, seq, kind + 128, b"", self.timeout)
            expected = 1
            while True:
                # Idle sessions can remain connected between user requests indefinitely.
                conn.settimeout(None)
                if not conn.recv(1, socket.MSG_PEEK):
                    return
                seq, kind, body = frame(conn, self.timeout)
                if kind != (4 if reduced else 2) or seq != expected:
                    raise ValueError("unexpected task identity")
                parse_started = time.perf_counter()
                args = task(body, len(self.source.layers), self.source.experts, self.index, self.count,
                            self.assignment, self.finite_check, reduced)
                started = time.perf_counter()
                with self.lock:
                    result = self.backend.compute(self.source, *args)
                    timings = self.backend.last_timings.copy() if self.profile and hasattr(self.backend, "last_timings") else {}
                computed = time.perf_counter()
                send(conn, seq, kind + 128, result, self.timeout)
                sent = time.perf_counter()
                self.requests += 1; self.entries += len(args[3])
                if self.verbose:
                    detail = ""
                    if self.profile:
                        timings.update(parse_ms=(started - parse_started) * 1000, send_ms=(sent - computed) * 1000)
                        detail = " " + " ".join(f"{name}={value:.3f}" for name, value in timings.items())
                    print(f"task={seq} layer={args[0]} entries={len(args[3])} "
                          f"compute_ms={(sent - started) * 1000:.3f} bytes={len(body) + len(result)}{detail}", flush=True)
                expected += 1
        except (EOFError, ConnectionError):
            pass
        except Exception as exc:
            try:
                send(conn, seq, 255, str(exc).encode("utf-8")[:1024], self.timeout)
            except OSError:
                pass
            print(f"worker session ended: {exc}", flush=True)
        finally:
            conn.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cluster-config", help="shared static startup plan; requires --node")
    parser.add_argument("--node", help="this machine's node id in the shared plan")
    parser.add_argument("--pack")
    parser.add_argument("--native", help="first GGUF shard; needed without experts.bin")
    parser.add_argument("--library", help="strata_network_backend.dll / libstrata_network_backend.so")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=18500)
    parser.add_argument("--api-key", help="default: STRATA_NETWORK_KEY environment variable")
    parser.add_argument("--allow-client", default="127.0.0.1", help="coordinator IPv4 address")
    parser.add_argument("--index", type=int)
    parser.add_argument("--count", type=int, default=2)
    parser.add_argument("--backend", choices=("cpu", "gpu"), default="cpu")
    parser.add_argument("--device", type=int, default=0)
    parser.add_argument("--workers", type=int, default=0)
    parser.add_argument("--gpu-cache-mib", type=int, default=4096)
    parser.add_argument("--ram-cache-mib", type=int, default=512)
    parser.add_argument("--preload-experts", action="store_true", help="load all assigned experts into RAM before listening")
    parser.add_argument("--timeout", type=float, default=60)
    args = parser.parse_args()
    plan = None
    if args.cluster_config:
        from cluster_config import load_cluster
        try:
            plan = load_cluster(args.cluster_config)
            node = plan.node(args.node)
            if node["role"] != "expert-worker":
                raise ValueError("this node is the coordinator; start it using tools/cluster.py")
            # Reject competing flags; one plan is authoritative for every deployment parameter.
            allowed = {"--cluster-config", "--node"}
            if any(arg.startswith("--") and arg.split("=", 1)[0] not in allowed for arg in sys.argv[1:]):
                raise ValueError("cluster startup accepts only --cluster-config and --node; edit the shared plan")
            for name in ("pack", "native", "library", "host", "port", "backend", "device", "workers",
                         "ram_cache_mib", "gpu_cache_mib"):
                setattr(args, name, node[name])
            args.index = plan.workers.index(node)
            args.count = len(plan.workers)
            args.allow_client = plan.coordinator["host"]
            args.preload_experts = node.get("preload_experts", False)
            args.timeout = max(1., plan.data["scheduler"]["timeout_ms"] / 1000.)
            print(f"cluster config_sha256={plan.digest}", flush=True)
        except (ValueError, OSError, KeyError) as error:
            parser.error(str(error))
    elif args.node:
        parser.error("--node requires --cluster-config")
    if args.pack is None or args.library is None or args.index is None:
        parser.error("provide --pack, --library and --index, or --cluster-config with --node")
    key = (args.api_key or os.environ.get("STRATA_NETWORK_KEY", "")).encode("utf-8")
    if not (16 <= len(key) <= 256 and 1 <= args.port <= 65535 and args.ram_cache_mib >= 64
            and 1 <= args.timeout <= 300 and args.workers >= 0 and args.device >= 0):
        parser.error("valid key, port, timeout, device, worker count, and RAM cache are required")
    ipaddress.IPv4Address(args.host)
    ipaddress.IPv4Address(args.allow_client)
    source = ExpertFiles(args.pack, args.native, cache_mib=args.ram_cache_mib)
    if plan and (len(source.layers), source.experts) != (plan.data["model"]["layers"], plan.data["model"]["experts"]):
        parser.error("local pack does not match the model geometry in the shared plan")
    backend = None
    try:
        backend = NativeBackend(args.library, args.workers, args.device if args.backend == "gpu" else -1, args.gpu_cache_mib)
        backend.validate(source)
        print("worker finite checks: " + ("native" if backend._finite is not None else "Python (older library)"), flush=True)
        worker = Worker(source, backend, key, args.index, args.count, args.timeout,
                        assignment=plan.assignment if plan else None, config_id=plan.digest if plan else None)
        if args.preload_experts:
            source.preload(args.index, args.count, worker.assignment, progress=lambda message: print(message, flush=True))
        # One active coordinator per worker. No requests are accepted until preload has completed.
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
            listener.bind((args.host, args.port))
            listener.listen(4)
            print(f"worker ready: {args.host}:{args.port} shard={args.index}/{args.count} "
                  f"backend={args.backend} fingerprint={worker.fingerprint:016x}", flush=True)
            while True:
                conn, peer = listener.accept()
                if peer[0] != args.allow_client:
                    conn.close()
                    continue
                worker.session(conn)
    except KeyboardInterrupt:
        pass
    except (ValueError, OSError, RuntimeError, MemoryError) as error:
        parser.error(f"worker startup failed: {error}")
    finally:
        if backend is not None:
            backend.close()
        source.close()


if __name__ == "__main__":
    main()
