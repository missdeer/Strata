"""LAN protocol failure tests and optional real CPU/GPU row parity (no model download)."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import socket
import queue
import struct
import subprocess
import threading
import time
import unittest

import network_worker as W

KEY = b"strata-network-test-key"


class Source:
    layers = [None, None]
    experts = 512
    def fingerprint(self):
        return 12345


class Backend:
    can_reduce = True
    def __init__(self):
        self.calls = 0
    def compute(self, source, layer, nt, k, entries, activation, weights=None):
        self.calls += 1
        if weights is not None:
            sums = [0.] * nt
            for (r, e), weight in zip(entries, weights):
                sums[r // k] += e * weight
            return b"".join(struct.pack("<f", v) * W.H for v in sums)
        return b"".join(struct.pack("<f", float(e)) * W.H for _, e in entries)


def hello(key=KEY, fingerprint=12345, index=0, count=2):
    return struct.pack("<QIIIII", fingerprint, 2, 512, index, count, W.H) + key


def body(entries=((0, 2), (1, 4)), nt=1, k=2, layer=0):
    return (struct.pack("<IIII", layer, nt, k, len(entries))
            + b"".join(struct.pack("<II", *entry) for entry in entries)
            + struct.pack("<f", .25) * (nt * W.H))


class ProtocolTests(unittest.TestCase):
    def setUp(self):
        self.backend = Backend()
        self.worker = W.Worker(Source(), self.backend, KEY, 0, 2, timeout=.3)
        self.conn, remote = socket.socketpair()
        self.thread = threading.Thread(target=self.worker.session, args=(remote,), daemon=True)
        self.thread.start()

    def tearDown(self):
        self.conn.close()
        self.thread.join(2)
        self.assertFalse(self.thread.is_alive(), "worker did not terminate disconnected session")

    def authenticate(self, reduced=False):
        kind = 5 if reduced else 1
        W.send(self.conn, 0, kind, hello(), 1)
        self.assertEqual(W.frame(self.conn, 1), (0, kind + 128, b""))

    def reduced_body(self, nt=1, weights=(.25, .75)):
        return (struct.pack("<IIII", 0, nt, 2, 2)
                + b"".join(struct.pack("<IIf", r, e, w) for r, e, w in zip((0, 1), (2, 4), weights))
                + struct.pack("<f", .25) * (nt * W.H))

    def test_reduced_response_and_empty_positions(self):
        self.authenticate(reduced=True)
        W.send(self.conn, 1, 4, self.reduced_body(nt=3), 1)
        seq, kind, result = W.frame(self.conn, 1)
        self.assertEqual((seq, kind), (1, 132))
        self.assertEqual(result, struct.pack("<f", 3.5) * W.H + b"\0" * (2 * W.H * 4))

    def test_reduced_nonfinite_weight_is_rejected(self):
        self.authenticate(reduced=True)
        W.send(self.conn, 1, 4, self.reduced_body(weights=(.25, float('nan'))), 1)
        self.assertIn(b"non-finite router weight", W.frame(self.conn, 1)[2])
        self.assertEqual(self.backend.calls, 0)

    def test_reduction_capability_required(self):
        self.backend.can_reduce = False
        W.send(self.conn, 0, 5, hello(), 1)
        self.assertIn(b"lacks reduced results support", W.frame(self.conn, 1)[2])

    def test_task_mode_cannot_change_after_handshake(self):
        self.authenticate(reduced=True)
        W.send(self.conn, 1, 2, body(), 1)
        self.assertIn(b"identity", W.frame(self.conn, 1)[2])
        self.assertEqual(self.backend.calls, 0)

    def reject_task(self, data, expected):
        self.authenticate()
        W.send(self.conn, 1, 2, data, 1)
        seq, kind, error = W.frame(self.conn, 1)
        self.assertEqual((seq, kind), (1, 255))
        self.assertIn(expected, error.decode())
        self.assertEqual(self.backend.calls, 0)

    def test_authentication_required(self):
        W.send(self.conn, 0, 1, hello(b"incorrect-network-key"), 1)
        self.assertIn(b"authentication failed", W.frame(self.conn, 1)[2])
        self.assertEqual(self.backend.calls, 0)

    def test_model_mismatch(self):
        W.send(self.conn, 0, 1, hello(fingerprint=999), 1)
        self.assertIn(b"mismatch", W.frame(self.conn, 1)[2])

    def test_shard_mismatch(self):
        W.send(self.conn, 0, 1, hello(index=1), 1)
        self.assertIn(b"mismatch", W.frame(self.conn, 1)[2])

    def test_fragmented_frame_and_row_order(self):
        self.authenticate()
        data = body(((1, 4), (0, 2)))
        packet = W.HEADER.pack(W.MAGIC, W.VERSION, 1, len(data), 2) + data
        for offset in range(0, len(packet), 117):
            self.conn.sendall(packet[offset:offset + 117])
        seq, kind, result = W.frame(self.conn, 1)
        self.assertEqual((seq, kind), (1, 130))
        self.assertEqual(struct.unpack_from("<f", result)[0], 4)
        self.assertEqual(struct.unpack_from("<f", result, W.H * 4)[0], 2)
        self.assertEqual(self.backend.calls, 1)

    def test_stale_sequence(self):
        self.authenticate()
        W.send(self.conn, 2, 2, body(), 1)
        self.assertIn(b"identity", W.frame(self.conn, 1)[2])
        self.assertEqual(self.backend.calls, 0)

    def test_unowned_expert(self):
        self.reject_task(body(((0, 3),)), "unowned")

    def test_duplicate_routing(self):
        self.reject_task(body(((0, 2), (1, 2))), "duplicate")

    def test_bad_row(self):
        self.reject_task(body(((2, 2),)), "routing")

    def test_bad_layer(self):
        self.reject_task(body(layer=2), "geometry")

    def test_wrong_size(self):
        self.reject_task(body()[:-1], "size")

    def test_non_finite_activation(self):
        data = body()
        self.reject_task(data[:-4] + struct.pack("<f", float("nan")), "non-finite")

    def test_oversized_frame(self):
        self.conn.sendall(W.HEADER.pack(W.MAGIC, W.VERSION, 0, W.MAX_BODY + 1, 1))
        self.assertIn(b"header", W.frame(self.conn, 1)[2])

    def test_partial_frame_timeout(self):
        self.conn.sendall(W.HEADER.pack(W.MAGIC, W.VERSION, 0, 28, 1)[:12])
        self.assertEqual(W.frame(self.conn, 1)[1], 255)

    def test_backend_error_is_propagated(self):
        self.authenticate()
        def failed(*args):
            raise RuntimeError("expert kernel failed")
        self.backend.compute = failed
        W.send(self.conn, 1, 2, body(), 1)
        self.assertIn(b"expert kernel failed", W.frame(self.conn, 1)[2])


def integration(args):
    build = Path(args.build).resolve()
    library = build / ("strata_network_backend.dll" if os.name == "nt" else "libstrata_network_backend.so")
    checker = build / ("strata-network-check.exe" if os.name == "nt" else "strata-network-check")
    sources, backends, listeners, threads, workers = [], [], [], [], []
    stop = threading.Event()
    try:
        peers = []
        for index in range(2):
            source = W.ExpertFiles(args.pack, args.native, cache_mib=64)
            sources.append(source)
            device = args.gpu_device if index == 1 and args.gpu_device is not None else -1
            backend = W.NativeBackend(library, workers=2, device=device, gpu_cache_mib=128)
            backends.append(backend)
            backend.validate(source)
            worker = W.Worker(source, backend, KEY, index, 2, timeout=5, verbose=False,
                              assignment=("0" * 256 + "1" * 256) if args.static_plan else None,
                              config_id="a" * 64 if args.static_plan else None)
            workers.append(worker)
            listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            listeners.append(listener)
            listener.bind(("127.0.0.1", 0)); listener.listen(2); listener.settimeout(.5)
            peers.append(f"127.0.0.1:{listener.getsockname()[1]}")
            def serve(listener=listener, worker=worker):
                while not stop.is_set():
                    try:
                        conn, _ = listener.accept()
                        worker.session(conn)
                    except socket.timeout:
                        continue
                    except OSError:
                        return
            thread = threading.Thread(target=serve, daemon=True)
            thread.start(); threads.append(thread)
        env = dict(os.environ, STRATA_NETWORK_KEY=KEY.decode())
        if args.reduce:
            env["STRATA_NETWORK_REDUCE"] = "1"
        if args.static_plan:
            env.update(STRATA_NETWORK_ASSIGNMENT="0" * 256 + "1" * 256, STRATA_NETWORK_CONFIG_ID="a" * 64)
        cmd = [str(checker), args.pack, ",".join(peers), str(len(sources[0].layers)), "-"]
        if args.native:
            cmd.append(args.native)
        result = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=60)
        print(result.stdout, end="")
        print(result.stderr, end="")
        if result.returncode:
            raise RuntimeError(f"C++ coordinator parity failed (exit {result.returncode})")
        print("real backend/C++ coordinator parity: PASS")
        coordinator_failures(checker, args, sources[0], env)
        if args.engine_config:
            engine_smoke(args.engine_config, ",".join(peers), env, workers, args.engine_spec, args.local_ram_mib)
    finally:
        stop.set()
        for listener in listeners:
            listener.close()
        for thread in threads:
            thread.join(6)
        # Do not free native contexts if a thread is still inside a computation.
        if any(thread.is_alive() for thread in threads):
            raise RuntimeError("integration worker still running")
        for backend in backends:
            backend.close()
        for source in sources:
            source.close()


def coordinator_failures(checker, args, source, env):
    env = dict(env)
    if "STRATA_NETWORK_ASSIGNMENT" in env:
        env["STRATA_NETWORK_ASSIGNMENT"] = "0" * 512
    for mode, expected in (("auth", "authentication failed"), ("model", "model mismatch"),
                           ("sequence", "identity"), ("disconnect", "disconnected"),
                           ("timeout", "timed out"), ("nan", "non-finite"), ("short", "result size")):
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.bind(("127.0.0.1", 0)); listener.listen(1); listener.settimeout(5)
        endpoint = f"127.0.0.1:{listener.getsockname()[1]}"
        def fake():
            try:
                conn, _ = listener.accept()
                with conn:
                    seq, kind, data = W.frame(conn, 3)
                    if mode in ("auth", "model"):
                        W.send(conn, seq, 255, expected.encode(), 3)
                        return
                    W.send(conn, seq, kind + 128, b"", 3)
                    seq, kind, data = W.frame(conn, 3)
                    if mode == "disconnect":
                        return
                    if mode == "timeout":
                        # Event wait is confined to this test thread, shorter than two seconds.
                        threading.Event().wait(1.3)
                        return
                    n = struct.unpack_from("<I", data, 4 if kind == 4 else 12)[0]
                    payload = struct.pack("<f", float("nan") if mode == "nan" else 0.) * (n * W.H)
                    if mode == "short":
                        payload = payload[:-4]
                    W.send(conn, seq + 1 if mode == "sequence" else seq, kind + 128, payload, 3)
            except (OSError, EOFError):
                pass
        thread = threading.Thread(target=fake, daemon=True)
        thread.start()
        cmd = [str(checker), args.pack, endpoint, str(len(source.layers)), expected]
        if args.native:
            cmd.append(args.native)
        try:
            result = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=8)
            if result.returncode:
                raise RuntimeError(f"coordinator {mode} failure test failed: {result.stderr}")
            print(f"C++ coordinator rejects {mode}: PASS")
        finally:
            listener.close(); thread.join(3)


def engine_smoke(config, peers, env, workers, spec, local_ram_mib=0):
    from batch_test import tokenizer
    cfg = json.loads(Path(config).read_text(encoding="utf-8"))
    env = dict(env)
    env["PATH"] = os.pathsep.join(cfg.get("lib_dirs", []) + [env.get("PATH", "")])
    env["STRATA_IQ_MT_MIN"] = "1"
    ids = tokenizer(cfg["tokenizer"]).encode(
        "<|im_start|>user\nWrite a Python function that merges overlapping intervals, then explain it.<|im_end|>\n"
        "<|im_start|>assistant\n<think>\n\n</think>\n\n", parse_special=True)
    base = list(cfg["args"])
    if spec == 0:
        while "--mtp" in base:
            at = base.index("--mtp")
            del base[at:at + 2]
    options = base + ["--network-workers", peers, "--network-timeout-ms", "5000",
        "--max-context", "4096", "--prefill", "512", "--expert-cache", "512", "--pcie-frac", "0",
        "--prompt-cache", "0", "--adapt-every", "0", "--spec", str(max(2, spec))]
    if env.get("STRATA_NETWORK_REDUCE") == "1":
        options.append("--network-reduce")
    if local_ram_mib:
        options += ["--network-local-ram-mib", str(local_ram_mib)]
    if "STRATA_NETWORK_ASSIGNMENT" in env:
        options += ["--network-assignment", env["STRATA_NETWORK_ASSIGNMENT"], "--network-config-id", env["STRATA_NETWORK_CONFIG_ID"]]
    path = Path(f"tmp/network-engine-smoke-spec{spec}.log")
    path.parent.mkdir(exist_ok=True)
    messages = queue.Queue()
    with path.open("w", encoding="utf-8") as log:
        process = subprocess.Popen([cfg["exe"], "--serve", *options], cwd=cfg.get("cwd"),
            env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, text=True, bufsize=1)
        def reader():
            for line in process.stdout:
                messages.put(line.rstrip())
            messages.put(None)
        thread = threading.Thread(target=reader, daemon=True)
        thread.start()
        def next_line(deadline):
            left = deadline - time.monotonic()
            if left <= 0:
                raise RuntimeError(f"engine deadline exceeded; see {path}")
            try:
                line = messages.get(timeout=left)
            except queue.Empty:
                raise RuntimeError(f"engine deadline exceeded; see {path}") from None
            if line is None:
                raise RuntimeError(f"engine ended (exit {process.poll()}); see {path}")
            if line.startswith("ERR"):
                raise RuntimeError(line)
            return line
        try:
            deadline = time.monotonic() + 180
            while not next_line(deadline).startswith("READY"):
                pass
            print("network engine READY", flush=True)
            for request in range(2):
                before = [w.requests for w in workers]
                process.stdin.write("GEN 8 temperature=0 " + ",".join(map(str, ids)) + "\n")
                process.stdin.flush()
                tokens = []
                deadline = time.monotonic() + 120
                while True:
                    line = next_line(deadline)
                    if line.startswith("T "):
                        tokens.append(int(line.split()[1]))
                    if line.startswith("DONE"):
                        break
                if len(tokens) < 8 or any(w.requests == n for w, n in zip(workers, before)):
                    raise RuntimeError("full-engine smoke did not generate or use both workers")
                print(f"full-engine spec={spec} request={request} tokens={tokens} worker_tasks="
                      f"{[w.requests - n for w, n in zip(workers, before)]}: PASS", flush=True)
            process.stdin.write("QUIT\n"); process.stdin.flush()
            process.wait(timeout=15)
            if process.returncode:
                raise RuntimeError(f"engine exit {process.returncode}")
            if local_ram_mib:
                import re
                counts = re.findall(r"strata network local: entries=(\d+)", path.read_text())
                if not counts or int(counts[-1]) == 0:
                    raise RuntimeError("local RAM mode did not execute any CPU expert")
                print(f"local RAM CPU entries={counts[-1]}: PASS", flush=True)
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait(timeout=5)
            thread.join(2)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", help="also run real C++/native integration using this build")
    parser.add_argument("--pack")
    parser.add_argument("--native")
    parser.add_argument("--gpu-device", type=int, help="second test worker uses this CUDA/HIP device")
    parser.add_argument("--engine-config", help="also run the complete engine on two localhost workers")
    parser.add_argument("--engine-spec", type=int, choices=(0, 4), default=4, help="full-engine MTP mode; 0 removes --mtp")
    parser.add_argument("--static-plan", action="store_true", help="test explicit ranges and configuration digest instead of legacy modulo")
    parser.add_argument("--reduce", action="store_true", help="test weighted compact LAN responses")
    parser.add_argument("--local-ram-mib", type=int, default=0, help="also verify local CPU participation in full-engine smoke")
    args = parser.parse_args()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(ProtocolTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful():
        raise SystemExit(1)
    if args.build:
        if not args.pack:
            parser.error("--build requires --pack")
        integration(args)
