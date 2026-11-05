"""Cross-check the native worker against the Python protocol/config/backend references.

No downloads or GPU are required. Protocol tests create a sparse native expert
pack; --pack/--native additionally compare real weights byte for byte.
Python is a test oracle only, never a dependency of the native worker.
"""
import argparse
import copy
import ctypes
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time
import unittest

import cluster_config as CFG
import network_worker as W
from test_network_experts import KEY, body

ROOT = Path(__file__).resolve().parents[1]
SETTINGS = None


def command(*args, env=None):
    return subprocess.run([str(SETTINGS.worker), "worker", *map(str, args)], capture_output=True,
                          encoding="utf-8", errors="replace", env=env, timeout=20)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class RunningWorker:
    def __init__(self, folder, *args, env=None):
        self.log_path = Path(folder) / ("worker-" + str(time.time_ns()) + ".log")
        self.log = self.log_path.open("wb")
        self.process = subprocess.Popen([str(SETTINGS.worker), "worker", *map(str, args)],
                                        stdout=self.log, stderr=self.log,
                                        env=env or dict(os.environ, STRATA_NETWORK_KEY=KEY.decode()))
        try:
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                if self.process.poll() is not None:
                    raise RuntimeError(self.log_path.read_text(encoding="utf-8", errors="replace"))
                if "worker ready:" in self.log_path.read_text(encoding="utf-8", errors="replace"):
                    return
                time.sleep(.02)
            raise RuntimeError("worker did not become ready")
        except BaseException:
            self.close()
            raise

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
        self.process.wait(timeout=10)
        self.log.close()


class ConfigParity(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="native-config-")
        self.folder = Path(self.temp.name)
        self.addCleanup(self.temp.cleanup)

    def check(self, data):
        path = self.folder / "共享配置.json"
        path.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8-sig")
        expected = CFG.ClusterConfig(data)
        result = command("--cluster-config", path, "--validate")
        self.assertEqual(result.returncode, 0, result.stderr)
        actual = json.loads(result.stdout)
        self.assertEqual(actual, dict(config_sha256=expected.digest, assignment=expected.assignment,
                                      endpoints=expected.endpoints))

    def test_shipped_plans_unicode_and_order(self):
        for name in ("cluster-2hosts.json", "cluster-3hosts.json", "cluster-4hosts.example.json"):
            data = json.loads((ROOT / "configs" / name).read_text())
            self.check(data)
            data["cluster_id"] = "集群😀\u007f\u0085"
            data["nodes"][1]["pack"] = "D:/模型/😀/专家"
            data["revision"] = 0xffffffff
            self.check(data)
            data = json.loads(json.dumps(data, sort_keys=True))
            data["nodes"].reverse()
            self.check(data)

    def test_invalid_plans_agree(self):
        base = json.loads((ROOT / "configs/cluster-3hosts.json").read_text())
        cases = []
        for group, key, value in (("scheduler", "timeout_ms", True), ("model", "experts", 511),
                                   ("model", "layers", 1.0), ("scheduler", "failover", True)):
            data = copy.deepcopy(base); data[group][key] = value; cases.append(data)
        for key, value in (("workers", True), ("ram_cache_mib", -1), ("gpu_cache_mib", 1 << 65),
                           ("online", 1), ("preload_experts", "yes"), ("expert_ranges", [[0, 256]]),
                           ("host", "127.00.0.1"), ("host", "127.0.0.1."), ("pack", "\u3000\u0085")):
            data = copy.deepcopy(base); data["nodes"][1][key] = value; cases.append(data)
        for key, value in (("network_reduce", 1), ("network_local_ram_mib", True)):
            data = copy.deepcopy(base); data["nodes"][0][key] = value; cases.append(data)
        data = copy.deepcopy(base); data["nodes"][2]["id"] = "host11"; cases.append(data)
        data = copy.deepcopy(base); data["nodes"][2]["online"] = False; cases.append(data)
        data = copy.deepcopy(base); data["nodes"][1]["expert_ranges"] = [[0, 254]]; cases.append(data)
        for data in cases:
            with self.subTest(data=data):
                with self.assertRaises(ValueError): CFG.ClusterConfig(data)
                path = self.folder / "invalid.json"
                path.write_text(json.dumps(data), encoding="utf-8")
                self.assertNotEqual(command("--cluster-config", path, "--validate").returncode, 0)

    def test_duplicate_keys_and_oversize(self):
        for text in ('{"revision":1,"revision":2}', '{"nodes":[{"id":"a","id":"b"}]}', " " * ((1 << 20) + 1)):
            path = self.folder / "bad.json"; path.write_text(text)
            result = command("--cluster-config", path, "--validate")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("duplicate JSON" if text.startswith("{") else "exceeds", result.stderr)


class ProtocolParity(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="native-protocol-")
        cls.folder = Path(cls.temp.name) / "模型😀"
        cls.folder.mkdir()
        cls.library = SETTINGS.library
        cls.backend = W.NativeBackend(cls.library, workers=2)
        cls.blob_bytes = cls.backend.lib.strata_network_format_bytes(22, 42)
        cls.pack = cls.folder / "pack"; cls.pack.mkdir()
        (cls.pack / "native_experts.txt").write_text(
            "# strata native experts v3\n0 22 42 0 " + str(cls.blob_bytes) + "\n")
        with (cls.pack / "experts.bin").open("wb") as f:
            if os.name == "nt":
                import msvcrt
                kernel = ctypes.WinDLL("kernel32", use_last_error=True)
                control = kernel.DeviceIoControl
                control.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32,
                                    ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32), ctypes.c_void_p]
                returned = ctypes.c_uint32()
                if not control(msvcrt.get_osfhandle(f.fileno()), 0x900c4, None, 0, None, 0, ctypes.byref(returned), None):
                    raise RuntimeError("cannot mark the test pack sparse")
            f.truncate(cls.blob_bytes * 512)
        cls.source = W.ExpertFiles(cls.pack, cache_mib=64)
        cls.fingerprint = cls.source.fingerprint()
        cls.port = free_port()
        cls.running = RunningWorker(cls.folder, "--pack", cls.pack, "--library", cls.library,
                                    "--index", 0, "--count", 1, "--port", cls.port, "--workers", 2,
                                    "--ram-cache-mib", 64, "--timeout-ms", 300)

    @classmethod
    def tearDownClass(cls):
        cls.running.close(); cls.source.close(); cls.backend.close(); cls.temp.cleanup()

    def connect(self, kind=1, key=KEY, fingerprint=None, seq=0, auth=True):
        conn = socket.create_connection(("127.0.0.1", self.port), timeout=3)
        self.addCleanup(conn.close)
        if auth:
            hello = struct.pack("<QIIIII", self.fingerprint if fingerprint is None else fingerprint, 1, 512, 0, 1, W.H) + key
            W.send(conn, seq, kind, hello, 3)
        return conn

    def authenticated(self, reduced=False):
        kind = 5 if reduced else 1
        conn = self.connect(kind=kind)
        self.assertEqual(W.frame(conn, 3), (0, kind + 128, b""))
        return conn

    def test_auth_model_and_sequence(self):
        for kwargs, error in ((dict(key=b"incorrect-network-key"), "authentication failed"),
                               (dict(fingerprint=1), "mismatch"), (dict(seq=1), "handshake required")):
            conn = self.connect(**kwargs)
            self.assertIn(error, W.frame(conn, 3)[2].decode())
            conn.close()

    def test_geometry_routing_and_nonfinite(self):
        data = body(((0, 2), (1, 4)))
        cases = [(b"short", "truncated task"), (body(layer=1), "geometry"), (body(nt=9), "geometry"),
                 (body(((0, 2), (0, 4))), "routing"), (body(((0, 2), (1, 2))), "routing"),
                 (body(((0, 512),)), "routing"), (data + b"!", "size"),
                 (data[:-4] + struct.pack("<f", float("inf")), "non-finite activation")]
        for data, error in cases:
            conn = self.authenticated()
            W.send(conn, 1, 2, data, 3)
            self.assertIn(error, W.frame(conn, 3)[2].decode())
            conn.close()

    def test_mode_sequence_and_oversize(self):
        for seq, kind in ((2, 2), (1, 4)):
            conn = self.authenticated(); W.send(conn, seq, kind, body(), 3)
            self.assertIn(b"identity", W.frame(conn, 3)[2]); conn.close()
        conn = self.connect(auth=False)
        conn.sendall(W.HEADER.pack(W.MAGIC, W.VERSION, 0, W.MAX_BODY + 1, 1))
        self.assertIn(b"header", W.frame(conn, 3)[2]); conn.close()

    def test_fragmented_raw_parity_and_cache_eviction(self):
        conn = self.authenticated()
        # More distinct experts than the 64 MiB cache holds. Revisit the first
        # task after eviction and compare every output byte with the reference.
        for seq, base in enumerate((0, 20, 40, 60, 0), 1):
            entries = [(i, base + i) for i in range(10)]
            data = body(entries, k=10)
            packet = W.HEADER.pack(W.MAGIC, W.VERSION, seq, len(data), 2) + data
            for at in range(0, len(packet), 117): conn.sendall(packet[at:at + 117])
            actual = W.frame(conn, 3)
            expected = self.backend.compute(self.source, *W.task(data, 1, 512, 0, 1))
            self.assertEqual(actual, (seq, 130, expected))
        conn.close()

    def test_reduced_parity_empty_positions_and_bad_weight(self):
        data = (struct.pack("<IIII", 0, 3, 2, 2) + struct.pack("<IIfIIf", 1, 4, .75, 0, 2, .25)
                + struct.pack("<f", .25) * (3 * W.H))
        conn = self.authenticated(reduced=True)
        W.send(conn, 1, 4, data, 3)
        expected = self.backend.compute(self.source, *W.task(data, 1, 512, 0, 1, reduced=True))
        self.assertEqual(W.frame(conn, 3), (1, 132, expected)); conn.close()
        conn = self.authenticated(reduced=True)
        data = data[:24] + struct.pack("<f", float("nan")) + data[28:]
        W.send(conn, 1, 4, data, 3)
        self.assertIn(b"non-finite router weight", W.frame(conn, 3)[2]); conn.close()

    def test_fragment_deadline_and_reconnect(self):
        conn = self.authenticated()
        conn.sendall(W.HEADER.pack(W.MAGIC, W.VERSION, 1, 100, 2) + b"!")
        self.assertIn(b"deadline", W.frame(conn, 3)[2]); conn.close()
        conn = self.authenticated(); conn.close()

    def test_idle_and_client_allowlist(self):
        conn = self.authenticated()
        time.sleep(.4)  # The idle interval may exceed the 300 ms frame deadline.
        W.send(conn, 1, 2, body(), 3)
        self.assertEqual(W.frame(conn, 3)[1], 130); conn.close()
        with socket.socket() as conn:
            conn.settimeout(3)
            conn.bind(("127.0.0.2", 0)); conn.connect(("127.0.0.1", self.port))
            self.assertEqual(conn.recv(1), b"")

    def test_no_python_on_path_and_unicode_key(self):
        port = free_port()
        key = "测试密钥-0123456789"
        env = dict(os.environ, STRATA_NETWORK_KEY=key)
        if os.name == "nt": env["PATH"] = str(Path(os.environ["SystemRoot"]) / "System32")
        else: env["PATH"] = "/nonexistent"
        running = RunningWorker(self.folder, "--pack", self.pack, "--library", self.library,
                                "--index", 0, "--count", 1, "--port", port, env=env)
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=3) as conn:
                hello = struct.pack("<QIIIII", self.fingerprint, 1, 512, 0, 1, W.H) + key.encode("utf-8")
                W.send(conn, 0, 1, hello, 3); self.assertEqual(W.frame(conn, 3)[1], 129)
                W.send(conn, 1, 2, body(), 3); self.assertEqual(W.frame(conn, 3)[1], 130)
        finally: running.close()

    def test_preload_rejected_before_listening(self):
        result = command("--pack", self.pack, "--library", self.library, "--index", 0, "--count", 1,
                         "--preload-experts", "--ram-cache-mib", 64,
                         env=dict(os.environ, STRATA_NETWORK_KEY=KEY.decode()))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("RAM cache budget", result.stderr)
        self.assertNotIn("worker ready:", result.stdout)

    def test_backend_failure_poisoning(self):
        library = self.library.parent / ("strata-network-test-backend.dll" if os.name == "nt" else "libstrata-network-test-backend.so")
        if not library.exists(): self.skipTest("build strata-network-test-backend with STRATA_BUILD_TESTS=ON")
        for mode in ("compute", "nonfinite", "reduce"):
            port = free_port()
            env = dict(os.environ, STRATA_NETWORK_KEY=KEY.decode(), STRATA_TEST_BACKEND_MODE=mode)
            running = RunningWorker(self.folder, "--pack", self.pack, "--library", library,
                                    "--index", 0, "--count", 1, "--port", port, env=env)
            try:
                for turn in range(2):
                    with socket.create_connection(("127.0.0.1", port), timeout=3) as conn:
                        kind = 5 if mode == "reduce" else 1
                        hello = struct.pack("<QIIIII", self.fingerprint, 1, 512, 0, 1, W.H) + KEY
                        W.send(conn, 0, kind, hello, 3); self.assertEqual(W.frame(conn, 3)[1], kind + 128)
                        task = body(((0, 2),), k=1)
                        if mode == "reduce": task = task[:24] + struct.pack("<f", 1.) + task[24:]
                        W.send(conn, 1, 4 if mode == "reduce" else 2, task, 3)
                        result = W.frame(conn, 3)
                        self.assertEqual(result[1], 255)
                        self.assertIn(b"restart the worker" if turn else
                                      b"non-finite results" if mode == "nonfinite" else b"injected backend failure", result[2])
            finally: running.close()

    def test_shared_plan_and_mismatches(self):
        data = json.loads((ROOT / "configs/cluster-2hosts.json").read_text())
        data["model"]["layers"] = 1
        data["nodes"] = data["nodes"][:2]
        data["nodes"][0]["host"] = "127.0.0.1"
        node = data["nodes"][1]
        port = free_port()
        node.update(host="127.0.0.2", port=port, pack=str(self.pack), native="unused.gguf", library=str(self.library),
                    workers=2, ram_cache_mib=64, preload_experts=False)
        plan = CFG.ClusterConfig(data)
        path = self.folder / "共享配置.json"; path.write_text(json.dumps(data), encoding="utf-8")
        running = RunningWorker(self.folder, "--cluster-config", path, "--node", "host11")
        try:
            for mismatch in (None, "digest", "assignment"):
                with socket.socket() as conn:
                    conn.bind(("127.0.0.1", 0)); conn.connect(("127.0.0.2", port))
                    hello = struct.pack("<QIIIII", self.fingerprint, 1, 512, 0, 1, W.H)
                    hello += ("a" * 64 if mismatch == "digest" else plan.digest).encode()
                    hello += ("1" * 512 if mismatch == "assignment" else plan.assignment).encode() + KEY
                    W.send(conn, 0, 3, hello, 3)
                    result = W.frame(conn, 3)
                    self.assertEqual(result[1], 255 if mismatch else 131)
                    if mismatch: self.assertIn(b"configuration mismatch", result[2])
        finally: running.close()

    def test_preload_complete_assignment(self):
        if W.available_memory_bytes()[0] < (9 << 30):
            self.skipTest("full preload test needs 9 GiB available RAM for its 8 GiB headroom")
        # Sixteen owners, 32 experts each: one complete assignment fits 64 MiB.
        data = json.loads((ROOT / "configs/cluster-2hosts.json").read_text())
        data["model"]["layers"] = 1
        data["nodes"] = data["nodes"][:1]
        data["nodes"][0]["host"] = "127.0.0.1"
        template = json.loads((ROOT / "configs/cluster-2hosts.json").read_text())["nodes"][1]
        port = free_port()
        for i in range(16):
            node = copy.deepcopy(template)
            node.update(id=f"worker{i:02}", host=f"127.0.0.{i + 2}", port=port, expert_ranges=[[i * 32, i * 32 + 31]],
                        pack=str(self.pack), native="unused.gguf", library=str(self.library), workers=2,
                        ram_cache_mib=64, preload_experts=True)
            data["nodes"].append(node)
        plan = CFG.ClusterConfig(data)
        path = self.folder / "preload.json"; path.write_text(json.dumps(data), encoding="utf-8")
        running = RunningWorker(self.folder, "--cluster-config", path, "--node", "worker00")
        try:
            log = running.log_path.read_text(encoding="utf-8")
            self.assertLess(log.index("preload complete:"), log.index("worker ready:"))
            self.assertIn(f"bytes={self.blob_bytes * 32}", log)
            with socket.socket() as conn:
                conn.bind(("127.0.0.1", 0)); conn.connect(("127.0.0.2", port))
                hello = struct.pack("<QIIIII", self.fingerprint, 1, 512, 0, 16, W.H) + plan.digest.encode() + plan.assignment.encode() + KEY
                W.send(conn, 0, 3, hello, 3); self.assertEqual(W.frame(conn, 3)[1], 131)
                W.send(conn, 1, 2, body(((0, 31),)), 3); self.assertEqual(W.frame(conn, 3)[1], 130)
                W.send(conn, 2, 2, body(((0, 32),)), 3); self.assertIn(b"unowned", W.frame(conn, 3)[2])
        finally: running.close()

    @unittest.skipUnless(os.name == "nt", "Windows mapping working-set regression")
    def test_preload_releases_source_mapping_pages(self):
        total = self.blob_bytes * 512
        if W.available_memory_bytes()[0] < total + (8 << 30) + (256 << 20):
            self.skipTest("full assignment needs its RAM cache plus 8 GiB headroom")
        running = RunningWorker(self.folder, "--pack", self.pack, "--library", self.library,
                                "--index", 0, "--count", 1, "--port", free_port(),
                                "--workers", 2, "--ram-cache-mib", (total + (1 << 20) - 1) >> 20,
                                "--preload-experts")
        try:
            # The cache owns copies, so completed preload must not keep another
            # whole assignment in the mapped-file working set.
            class Counters(ctypes.Structure):
                _fields_ = [("cb", ctypes.c_uint32), ("faults", ctypes.c_uint32)] + [
                    (name, ctypes.c_size_t) for name in ("peak_rss", "rss", "peak_paged", "paged",
                                                       "peak_nonpaged", "nonpaged", "pagefile", "peak_pagefile")]
            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
            kernel.OpenProcess.restype = ctypes.c_void_p
            kernel.CloseHandle.argtypes = [ctypes.c_void_p]
            handle = kernel.OpenProcess(0x410, False, running.process.pid)
            self.assertTrue(handle, ctypes.get_last_error())
            try:
                info = Counters(); info.cb = ctypes.sizeof(info)
                query = ctypes.WinDLL("psapi", use_last_error=True).GetProcessMemoryInfo
                query.argtypes = [ctypes.c_void_p, ctypes.POINTER(Counters), ctypes.c_uint32]
                self.assertTrue(query(handle, ctypes.byref(info), info.cb), ctypes.get_last_error())
                self.assertLess(info.rss, total + (128 << 20), (info.rss, total))
            finally:
                kernel.CloseHandle(handle)
        finally:
            running.close()


def real_parity():
    if not SETTINGS.pack: return
    source = W.ExpertFiles(SETTINGS.pack, SETTINGS.native, cache_mib=64)
    backend = W.NativeBackend(SETTINGS.library, workers=2,
                              device=SETTINGS.gpu_device if SETTINGS.gpu_device is not None else -1)
    with tempfile.TemporaryDirectory(prefix="native-real-") as folder:
        port = free_port()
        gpu_args = ["--backend", "gpu", "--device", SETTINGS.gpu_device] if SETTINGS.gpu_device is not None else []
        running = RunningWorker(folder, "--pack", SETTINGS.pack, "--native", SETTINGS.native,
                                "--library", SETTINGS.library, "--index", 0, "--count", 1,
                                "--port", port, "--workers", 2, "--ram-cache-mib", 64, *gpu_args)
        try:
            formats = {}
            for layer, fmt in enumerate(source.layers): formats.setdefault(fmt[:2], layer)
            for reduced in (False, True):
                with socket.create_connection(("127.0.0.1", port), timeout=10) as conn:
                    kind = 5 if reduced else 1
                    hello = struct.pack("<QIIIII", source.fingerprint(), len(source.layers), 512, 0, 1, W.H) + KEY
                    W.send(conn, 0, kind, hello, 10)
                    assert W.frame(conn, 10) == (0, kind + 128, b"")
                    seq = 0
                    for layer in formats.values():
                        for nt in (1, 3, 8):
                            seq += 1
                            entries = [(r, r % 2 + 2) for r in range(nt * 2)]
                            if reduced:
                                data = (struct.pack("<IIII", layer, nt, 2, len(entries)) +
                                        b"".join(struct.pack("<IIf", r, e, .25 + (r % 2) * .5) for r, e in entries) +
                                        struct.pack("<f", .25) * (nt * W.H))
                            else: data = body(entries, nt=nt, k=2, layer=layer)
                            W.send(conn, seq, 4 if reduced else 2, data, 10)
                            actual = W.frame(conn, 10)
                            expected = backend.compute(source, *W.task(data, len(source.layers), 512, 0, 1, reduced=reduced))
                            assert actual == (seq, 132 if reduced else 130, expected), (layer, nt, reduced)
            checker = SETTINGS.checker or SETTINGS.worker.parent / ("strata-network-check.exe" if os.name == "nt" else "strata-network-check")
            for reduced in (False, True):
                env = dict(os.environ, STRATA_NETWORK_KEY=KEY.decode())
                if reduced: env["STRATA_NETWORK_REDUCE"] = "1"
                else: env.pop("STRATA_NETWORK_REDUCE", None)
                env.pop("STRATA_NETWORK_ASSIGNMENT", None); env.pop("STRATA_NETWORK_CONFIG_ID", None)
                result = subprocess.run([str(checker), SETTINGS.pack, f"127.0.0.1:{port}", str(len(source.layers)), "-", SETTINGS.native],
                                        env=env, capture_output=True, text=True, timeout=60)
                if result.returncode: raise RuntimeError(result.stdout + result.stderr)
                print(result.stdout, end="")
            print(f"real pack: {len(formats) * 3 * 2} byte-identical Python/native cases; C++ coordinator raw/reduced: PASS")
        finally:
            running.close(); source.close(); backend.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--worker", required=True, type=Path)
    parser.add_argument("--library", required=True, type=Path)
    parser.add_argument("--pack")
    parser.add_argument("--native", default="")
    parser.add_argument("--gpu-device", type=int, help="also exercise the real worker's CUDA/HIP backend")
    parser.add_argument("--checker", type=Path, help="C++ coordinator check executable when testing a runtime package")
    SETTINGS = parser.parse_args()
    SETTINGS.worker = SETTINGS.worker.resolve(); SETTINGS.library = SETTINGS.library.resolve()
    suite = unittest.TestSuite([unittest.defaultTestLoader.loadTestsFromTestCase(c) for c in (ConfigParity, ProtocolParity)])
    if not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful(): raise SystemExit(1)
    real_parity()
