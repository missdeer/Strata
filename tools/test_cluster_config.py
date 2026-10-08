"""Static cluster plan validation, task ownership, handshake consistency and real C++ routing tests."""
import argparse
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.request

from cluster_config import ClusterConfig, load_cluster
import network_worker as W
from test_network_experts import KEY, Source, Backend, body
from make_network_config import make_config

ROOT = Path(__file__).resolve().parents[1]


def base():
    return json.loads((ROOT / "configs/cluster-3hosts.json").read_text(encoding="utf-8"))


class PlanTests(unittest.TestCase):
    def test_two_hosts(self):
        plan = load_cluster(ROOT / "configs/cluster-2hosts.json")
        self.assertEqual(plan.assignment, "0" * 512)
        self.assertEqual(len(plan.workers), 1)
        with self.assertRaisesRegex(ValueError, "offline"):
            plan.node("host17")

    def test_three_hosts(self):
        plan = ClusterConfig(base())
        self.assertEqual(plan.assignment, "0" * 256 + "1" * 256)
        self.assertEqual(plan.node("host17")["backend"], "gpu")

    def test_four_hosts(self):
        plan = load_cluster(ROOT / "configs/cluster-4hosts.example.json")
        self.assertEqual(plan.assignment, "0" * 256 + "1" * 128 + "2" * 128)

    def test_overlap_rejected(self):
        data = base(); data["nodes"][2]["expert_ranges"] = [[255, 511]]
        with self.assertRaisesRegex(ValueError, "multiple"):
            ClusterConfig(data)

    def test_gap_rejected(self):
        data = base(); data["nodes"][2]["expert_ranges"] = [[257, 511]]
        with self.assertRaisesRegex(ValueError, "no online owner"):
            ClusterConfig(data)

    def test_switching_off_requires_reassignment(self):
        data = base(); data["nodes"][2]["online"] = False
        with self.assertRaisesRegex(ValueError, "no online owner"):
            ClusterConfig(data)
        data["nodes"][1]["expert_ranges"] = [[0, 511]]
        self.assertEqual(ClusterConfig(data).assignment, "0" * 512)

    def test_changed_budget_changes_digest(self):
        data = base(); first = ClusterConfig(data).digest
        data["nodes"][1]["workers"] = 6
        self.assertNotEqual(first, ClusterConfig(data).digest)

    def test_formatting_and_object_key_order_do_not_change_digest(self):
        data = base()
        self.assertEqual(ClusterConfig(data).digest, ClusterConfig(json.loads(json.dumps(data, sort_keys=True))).digest)

    def test_stable_node_ids_define_wire_order(self):
        data = base(); data["nodes"].reverse()
        self.assertEqual(ClusterConfig(data).assignment, "0" * 256 + "1" * 256)

    def test_duplicate_node_id(self):
        data = base(); data["nodes"][2]["id"] = "host11"
        with self.assertRaisesRegex(ValueError, "duplicate"):
            ClusterConfig(data)

    def test_unknown_setting(self):
        data = base(); data["scheduler"]["failover"] = True
        with self.assertRaises(ValueError):
            ClusterConfig(data)

    def test_existing_runtime_network_flags_are_replaced(self):
        cfg = {"args": ["--native", "model.gguf", "--network-assignment", "old", "--network-config-id", "old",
                        "--layer-split", "16", "--network-workers", "1.2.3.4:18500"], "gpu": [0, 1]}
        new = make_config(cfg, "172.16.0.11:18500")
        self.assertNotIn("--network-assignment", new["args"])
        self.assertNotIn("--layer-split", new["args"])
        self.assertEqual(new["args"].count("--network-workers"), 1)
        self.assertEqual(cfg["gpu"], [0, 1])

    def test_bool_is_not_numeric_budget(self):
        data = base(); data["nodes"][1]["workers"] = True
        with self.assertRaises(ValueError):
            ClusterConfig(data)

    def test_reject_duplicate_json_keys(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "plan.json"
            path.write_text('{"revision":1,"revision":2}')
            with self.assertRaisesRegex(ValueError, "duplicate JSON"):
                load_cluster(path)

    def test_explicit_range_ownership(self):
        owners = [0] * 256 + [1] * 256
        # Odd id 3 belongs to host11 in this plan; this deliberately differs from old modulo routing.
        W.task(body(((0, 3),)), 2, 512, 0, 2, owners)
        with self.assertRaisesRegex(ValueError, "unowned"):
            W.task(body(((0, 3),)), 2, 512, 1, 2, owners)

    def test_handshake_digest_and_assignment(self):
        for mismatch in (None, "digest", "assignment", "legacy"):
            assignment, digest = "0" * 256 + "1" * 256, "a" * 64
            worker = W.Worker(Source(), Backend(), KEY, 0, 2, timeout=.5, verbose=False,
                              assignment=assignment, config_id=digest)
            conn, remote = socket.socketpair()
            thread = threading.Thread(target=worker.session, args=(remote,), daemon=True)
            thread.start()
            try:
                hello = struct.pack("<QIIIII", 12345, 2, 512, 0, 2, W.H)
                if mismatch != "legacy":
                    hello += ("b" * 64 if mismatch == "digest" else digest).encode()
                    hello += ("1" * 512 if mismatch == "assignment" else assignment).encode()
                hello += KEY
                W.send(conn, 0, 1 if mismatch == "legacy" else 3, hello, 1)
                seq, kind, error = W.frame(conn, 1)
                if mismatch:
                    self.assertEqual(kind, 255)
                    self.assertIn("mismatch" if mismatch != "legacy" else "handshake", error.decode())
                else:
                    self.assertEqual(kind, 131)
                    W.send(conn, 1, 2, body(((0, 3),)), 1)
                    self.assertEqual(W.frame(conn, 1)[1], 130)
            finally:
                conn.close(); thread.join(2)
                self.assertFalse(thread.is_alive())


def integration(args):
    build = Path(args.build).resolve()
    library = build / ("strata_network_backend.dll" if os.name == "nt" else "libstrata_network_backend.so")
    checker = build / ("strata-network-check.exe" if os.name == "nt" else "strata-network-check")
    source = W.ExpertFiles(args.pack, args.native, cache_mib=64)
    try:
        # One, two, three, and four workers validate extension beyond the original three-worker limit.
        for count in (1, 2, 3, 4):
            owners = "".join("0123456789abcdef"[min(count - 1, e * count // 512)] for e in range(512))
            digest = "a" * 64
            listeners, threads, backends, workers, sources = [], [], [], [], []
            try:
                endpoints = []
                for index in range(count):
                    local_source = W.ExpertFiles(args.pack, args.native, cache_mib=64)
                    sources.append(local_source)
                    backend = W.NativeBackend(library, workers=1)
                    backends.append(backend)
                    worker = W.Worker(local_source, backend, KEY, index, count, timeout=3, verbose=False,
                                      assignment=owners, config_id=digest)
                    workers.append(worker)
                    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                    listeners.append(listener)
                    listener.bind(("127.0.0.1", 0)); listener.listen(1); listener.settimeout(5)
                    endpoints.append(f"127.0.0.1:{listener.getsockname()[1]}")
                    def serve(listener=listener, worker=worker):
                        try:
                            conn, _ = listener.accept(); worker.session(conn)
                        except OSError:
                            pass
                    thread = threading.Thread(target=serve, daemon=True)
                    thread.start(); threads.append(thread)
                env = dict(os.environ, STRATA_NETWORK_KEY=KEY.decode(), STRATA_NETWORK_ASSIGNMENT=owners,
                           STRATA_NETWORK_CONFIG_ID=digest)
                cmd = [str(checker), args.pack, ",".join(endpoints), str(len(source.layers)), "-"]
                if args.native:
                    cmd.append(args.native)
                result = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=30)
                if result.returncode or any(worker.requests == 0 for worker in workers):
                    raise RuntimeError(f"static routing with {count} workers failed: {result.stdout} {result.stderr}")
                print(f"C++ static assignment, {count + 1} hosts: PASS; tasks={[w.requests for w in workers]}")
            finally:
                for listener in listeners:
                    listener.close()
                for thread in threads:
                    thread.join(4)
                if any(t.is_alive() for t in threads):
                    raise RuntimeError("worker thread did not stop")
                for backend in backends:
                    backend.close()
                for local_source in sources:
                    local_source.close()
    finally:
        source.close()
    startup(args, library, checker)


def startup(args, library, checker):
    # Launch the real worker entry point from a complete shared JSON, rather than constructing Worker in the test.
    data = base()
    data["nodes"][0]["host"] = "127.0.0.1"
    processes, logs = [], []
    with tempfile.TemporaryDirectory(prefix="strata-plan-startup-", ignore_cleanup_errors=True) as folder:
        plan_path = Path(folder) / "cluster.json"
        server_log = Path(folder) / "coordinator.log"
        if args.engine_config:
            server = json.loads(Path(args.engine_config).read_text(encoding="utf-8"))
            with socket.socket() as probe:
                probe.bind(("127.0.0.1", 0)); server["port"] = probe.getsockname()[1]
            server.update(open_browser=False, api_key=KEY.decode())
            server["args"] += ["--expert-cache", "512", "--prompt-cache", "0", "--adapt-every", "0"]
            source_path = Path(folder) / "source.json"
            source_path.write_text(json.dumps(server), encoding="utf-8")
            data["nodes"][0].update(server_config=str(source_path), engine=server["exe"], context=4096)
        for index, node in enumerate(data["nodes"][1:]):
            node.update(host=f"127.0.0.{index + 2}", backend="cpu", workers=1, ram_cache_mib=64,
                        pack=str(Path(args.pack).resolve()), native=str(Path(args.native).resolve()) if args.native else node["native"],
                        library=str(library))
            with socket.socket() as probe:
                probe.bind((node["host"], 0)); node["port"] = probe.getsockname()[1]
        plan_path.write_text(json.dumps(data), encoding="utf-8")
        plan = load_cluster(plan_path)
        env = dict(os.environ, STRATA_NETWORK_KEY=KEY.decode(), STRATA_NETWORK_ASSIGNMENT=plan.assignment,
                   STRATA_NETWORK_CONFIG_ID=plan.digest)
        try:
            for node in plan.workers:
                path = Path(folder) / (node["id"] + ".log")
                log = path.open("w", encoding="utf-8"); logs.append(log)
                process = subprocess.Popen([sys.executable, str(ROOT / "tools/network_worker.py"),
                    "--cluster-config", str(plan_path), "--node", node["id"]], stdout=log, stderr=log, env=env, cwd=ROOT)
                processes.append(process)
                deadline = time.monotonic() + 10
                while "worker ready:" not in path.read_text(encoding="utf-8"):
                    if process.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError("worker config startup failed: " + path.read_text(encoding="utf-8"))
                    threading.Event().wait(.05)
            cmd = [str(checker), args.pack, plan.endpoints, str(plan.data["model"]["layers"]), "-"]
            if args.native:
                cmd.append(args.native)
            result = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=15)
            if result.returncode:
                raise RuntimeError(f"worker CLI shared-plan parity failed: {result.stdout} {result.stderr}")
            print("worker CLI reads identical JSON, validates digest and computes IQ3_S: PASS")
            if args.engine_config:
                log = server_log.open("w", encoding="utf-8"); logs.append(log)
                process = subprocess.Popen([sys.executable, str(ROOT / "tools/cluster.py"),
                    "--config", str(plan_path), "--node", "host16"], stdout=log, stderr=log, env=env, cwd=ROOT)
                processes.insert(0, process)  # End the coordinator before its workers during cleanup.
                deadline = time.monotonic() + 180
                while "ready: http://" not in server_log.read_text(encoding="utf-8"):
                    if process.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError("coordinator config startup failed: " + server_log.read_text(encoding="utf-8"))
                    threading.Event().wait(.1)
                request = urllib.request.Request(f"http://127.0.0.1:{server['port']}/v1/chat/completions",
                    data=json.dumps({"model": "qwen3.8-flash-next-iq3_s-network", "messages": [{"role": "user",
                        "content": "Write a Python function that merges overlapping intervals."}],
                        "max_tokens": 8, "temperature": 0}).encode(),
                    headers={"Content-Type": "application/json", "Authorization": "Bearer " + KEY.decode()})
                with urllib.request.urlopen(request, timeout=120) as response:
                    payload = json.load(response)
                if payload.get("usage", {}).get("completion_tokens", 0) < 1:
                    raise RuntimeError(f"shared-plan API returned no generated tokens: {payload}")
                print(f"coordinator launcher + shared JSON + real IQ3_S HTTP generation: PASS; "
                      f"completion_tokens={payload['usage']['completion_tokens']}")
        finally:
            for process in processes:
                if process.poll() is None:
                    process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait(timeout=5)
            for log in logs:
                log.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build")
    parser.add_argument("--pack")
    parser.add_argument("--native")
    parser.add_argument("--engine-config", help="also start the real coordinator launcher and generate through its API")
    args = parser.parse_args()
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(PlanTests))
    if not result.wasSuccessful():
        raise SystemExit(1)
    if args.build:
        if not args.pack:
            parser.error("--build requires --pack")
        integration(args)
