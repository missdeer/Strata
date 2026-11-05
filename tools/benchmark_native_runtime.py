"""Stage 6 measurement clients. Python is a test driver, never a runtime dependency.

Run each client against isolated Python/native services on the same day, using
the same configuration and backend. This tool does not restart installed services.
Reports contain samples, output hashes and measurement boundaries, not speed claims.
"""
from __future__ import annotations

import argparse
import ctypes
from datetime import datetime, timezone
import hashlib
import http.client
import json
import math
import os
from pathlib import Path
import re
import socket
import statistics
import struct
import time


def summary(values):
    values = sorted(values)
    if not values:
        return None
    return {"count": len(values), "p50": statistics.median(values),
            "p95": values[math.ceil(.95 * len(values)) - 1],
            "min": values[0], "max": values[-1]}


def save(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")
    temporary.replace(path)


def report_base(args):
    return {"measured_at_utc": datetime.now(timezone.utc).isoformat(),
            "label": args.label, "python_is_test_driver_only": True,
            "notes": [], "samples": []}


def experts(args):
    import network_worker as W
    from cluster_config import load_cluster
    from gguf_reader import GGML_TYPES

    key = os.environ.get("STRATA_NETWORK_KEY", "").encode("utf-8")
    if not 16 <= len(key) <= 256:
        raise ValueError("set STRATA_NETWORK_KEY (16..256 UTF-8 bytes)")
    plan = load_cluster(args.cluster_config)
    node = plan.node(args.node)
    if node["role"] != "expert-worker":
        raise ValueError("--node must identify an expert-worker")
    # Select only locally assigned experts, including for split-node plans.
    index = next(i for i, worker in enumerate(plan.workers) if worker["id"] == args.node)
    owned = [e for e in range(512) if int(plan.assignment[e], 16) == index]
    if len(owned) < 48:
        raise ValueError("benchmark shapes require at least 48 assigned experts")
    source = W.ExpertFiles(args.pack, args.native, cache_mib=128)
    try:
        formats = {}
        for layer, fmt in enumerate(source.layers):
            formats.setdefault(fmt[:2], layer)
        hello = (struct.pack("<QIIIII", source.fingerprint(), len(source.layers), 512,
                             index, len(plan.workers), W.H)
                 + plan.digest.encode() + plan.assignment.encode() + key)
    finally:
        source.close()
    report = report_base(args)
    report.update(config_sha256=plan.digest, endpoint=f'{node["host"]}:{node["port"]}',
                  warmup=args.warmup, repeats=args.repeats, groups=[],
                  protocol_header_bytes=W.HEADER.size, logging_profile=os.environ.get("STRATA_NETWORK_PROFILE", "0"))
    report["notes"] = ["TCP v1 application bytes include the frame header, exclude TCP/IP overhead.",
                       "Round-trip includes computation, validation, scheduling and transport.",
                       "Each fixed shape is warmed before measurement; this is not decode tok/s.",
                       "Compare output_sha256 against the matching Python/native report."]
    for reduced in (False, True):
        seq = 0
        with socket.create_connection((node["host"], node["port"]), timeout=10) as conn:
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            W.send(conn, 0, 6 if reduced else 3, hello, 60)
            if W.frame(conn, 60) != (0, 134 if reduced else 131, b""):
                raise AssertionError("authenticated shared-plan handshake rejected")
            for fmt, layer in formats.items():
                for nt, shared in ((1, True), (3, True), (3, False), (8, False)):
                    entries = [(t * 6 + j, owned[j if shared else t * 6 + j])
                               for t in range(nt) for j in range(6)]
                    body = (struct.pack("<IIII", layer, nt, 6, len(entries))
                            + b"".join(struct.pack("<IIf", row, expert, 1 / 6) if reduced
                                       else struct.pack("<II", row, expert) for row, expert in entries)
                            + struct.pack("<f", .125) * (nt * W.H))
                    group = {"layer": layer, "formats": [GGML_TYPES[t] for t in fmt],
                             "positions": nt, "shared": shared, "reduced": reduced,
                             "tx_bytes_per_task": W.HEADER.size + len(body)}
                    samples, first_hash = [], None
                    for iteration in range(args.warmup + args.repeats):
                        seq += 1
                        started = time.perf_counter()
                        W.send(conn, seq, 4 if reduced else 2, body, 60)
                        reply_seq, kind, reply = W.frame(conn, 60)
                        elapsed = (time.perf_counter() - started) * 1000
                        if (reply_seq, kind) != (seq, 132 if reduced else 130):
                            raise AssertionError(f"invalid task response ({reply_seq}, {kind}): {reply[:256]!r}")
                        if len(reply) != (nt if reduced else len(entries)) * W.H * 4:
                            raise AssertionError("invalid task response size")
                        if not all(math.isfinite(v[0]) for v in struct.iter_unpack("<f", reply)):
                            raise AssertionError("nonfinite task output")
                        digest = hashlib.sha256(reply).hexdigest()
                        if first_hash is not None and digest != first_hash:
                            raise AssertionError("fixed task returned a different output")
                        first_hash = digest
                        if iteration >= args.warmup:
                            samples.append(elapsed)
                    group.update(roundtrip_ms=summary(samples), samples_ms=samples,
                                 rx_bytes_per_task=W.HEADER.size + len(reply), output_sha256=first_hash)
                    report["groups"].append(group)
                    save(args.report, report)
                    print(f'{args.label}: layer={layer} nt={nt} shared={shared} reduce={reduced}', flush=True)
    report["measurement_finished_at_utc"] = datetime.now(timezone.utc).isoformat()
    save(args.report, report)


def request(port, path):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=900)
    try:
        conn.request("GET", path)
        res = conn.getresponse()
        data = res.read()
        if res.status != 200:
            raise AssertionError(f"{path}: HTTP {res.status}")
        return json.loads(data)
    finally:
        conn.close()


def network_counts(path):
    # Engine reports cumulative counters after generation, one per endpoint.
    counters = {}
    if path:
        for endpoint, tasks, entries, tx, rx in re.findall(
                r"strata network: (\S+) requests=(\d+) entries=(\d+) tx=(\d+) rx=(\d+)",
                path.read_text(encoding="utf-8", errors="replace")):
            counters[endpoint] = dict(tasks=int(tasks), entries=int(entries), tx=int(tx), rx=int(rx))
    return counters


def api(args):
    report = report_base(args)
    report.update(warmup=args.warmup, repeats=args.repeats, groups=[],
                  engine=request(args.port, "/metrics").get("engine", {}))
    report["notes"] = ["TTFT is time to first nonempty content delta, including prefill and transport.",
                       "Decode rate is the engine's predicted_per_second from /v1/status.",
                       "HTTP body bytes exclude HTTP headers and TCP/IP overhead.",
                       "Identical prompts are repeated with the configured session/prompt cache.",
                       "Warmups are excluded; output hashes and usage must match the other runtime."]
    prompts = [("code", "Write a Python function implementing binary search on a sorted list. Include type hints and handle empty input. Return only code.", args.max_tokens),
               ("long", "Context notes:\n" + "The sky is blue and grass is green.\n" * 200
                + "\nWhat color is the sky? Answer in five words or fewer.", 32)]
    model = request(args.port, "/v1/models")["data"][0]["id"]
    for name, prompt, limit in prompts:
        samples = []
        for iteration in range(args.warmup + args.repeats):
            body = json.dumps({"model": model, "messages": [{"role": "user", "content": prompt}],
                               "temperature": 0, "reasoning_effort": "none", "max_tokens": limit,
                               "stream": True, "stream_options": {"include_usage": True}}).encode()
            conn = http.client.HTTPConnection("127.0.0.1", args.port, timeout=900)
            before = network_counts(args.engine_log)
            started_at = datetime.now(timezone.utc).isoformat()
            started = time.perf_counter()
            text, usage, finish, ttft, rx_bytes, done = [], None, None, None, 0, False
            try:
                conn.request("POST", "/v1/chat/completions", body, {"Content-Type": "application/json"})
                res = conn.getresponse()
                if res.status != 200:
                    raise AssertionError(f"HTTP {res.status}: {res.read()[:500]!r}")
                while True:
                    line = res.readline()
                    rx_bytes += len(line)
                    if not line:
                        break
                    if not line.startswith(b"data: "):
                        continue
                    data = line[6:].strip()
                    if data == b"[DONE]":
                        done = True
                        break
                    event = json.loads(data)
                    if "error" in event:
                        raise AssertionError("SSE returned an error")
                    if event.get("usage"):
                        usage = event["usage"]
                    for choice in event.get("choices", []):
                        content = choice.get("delta", {}).get("content")
                        if content:
                            if ttft is None:
                                ttft = (time.perf_counter() - started) * 1000
                            text.append(content)
                        finish = choice.get("finish_reason") or finish
            finally:
                conn.close()
            wall_ms = (time.perf_counter() - started) * 1000
            finished_at = datetime.now(timezone.utc).isoformat()
            if not done or usage is None or ttft is None or finish is None:
                raise AssertionError("incomplete SSE generation")
            timing = request(args.port, "/v1/status")["last_timings"]
            if not timing or not timing.get("predicted_per_second"):
                raise AssertionError("engine did not provide decode timing")
            # The API completion and final log flush can be on different threads.
            after = network_counts(args.engine_log)
            delta = {host: {k: counts[k] - before.get(host, {}).get(k, 0) for k in counts}
                     for host, counts in after.items()}
            sample = {"ttft_ms": ttft, "wall_ms": wall_ms, "timings": timing,
                      "started_at_utc": started_at, "finished_at_utc": finished_at,
                      "usage": usage, "finish": finish, "http_tx_body_bytes": len(body),
                      "http_rx_body_bytes": rx_bytes, "network_counters_delta": delta,
                      "output_sha256": hashlib.sha256("".join(text).encode()).hexdigest(),
                      "output_text": "".join(text),
                      "warmup": iteration < args.warmup}
            report["samples"].append({"prompt": name, **sample})
            if iteration >= args.warmup:
                samples.append(sample)
            save(args.report, report)
            print(f'{args.label}: {name} {iteration + 1}: {wall_ms / 1000:.3f}s', flush=True)
        report["groups"].append({"prompt": name, "request_max_tokens": limit,
                                 "ttft_ms": summary([s["ttft_ms"] for s in samples]),
                                 "wall_ms": summary([s["wall_ms"] for s in samples]),
                                 "decode_tok_s": summary([s["timings"]["predicted_per_second"] for s in samples])})
        save(args.report, report)
    report["measurement_finished_at_utc"] = datetime.now(timezone.utc).isoformat()
    save(args.report, report)


class Nvidia:
    """Optional driver sampling; no nvidia-smi subprocess or Python NVML package."""
    def __init__(self):
        try:
            self.lib = ctypes.CDLL("nvml.dll" if os.name == "nt" else "libnvidia-ml.so.1")
            if self.lib.nvmlInit_v2() != 0:
                raise RuntimeError("nvmlInit_v2 failed")
            self.device = ctypes.c_void_p()
            if self.lib.nvmlDeviceGetHandleByIndex_v2(0, ctypes.byref(self.device)) != 0:
                raise RuntimeError("NVML device 0 unavailable")
            self.error = None
        except (OSError, AttributeError, RuntimeError) as exc:
            self.error = str(exc)

    def sample(self):
        if self.error:
            return None
        memory = (ctypes.c_ulonglong * 3)()
        utilization = (ctypes.c_uint * 2)()
        if (self.lib.nvmlDeviceGetMemoryInfo(self.device, ctypes.byref(memory)) != 0
                or self.lib.nvmlDeviceGetUtilizationRates(self.device, ctypes.byref(utilization)) != 0):
            raise RuntimeError("NVML sample failed")
        return {"vram_used_bytes": memory[2], "gpu_percent": utilization[0]}


def sample_process(args):
    import psutil

    root = psutil.Process(args.pid)
    identity = root.create_time()
    report = report_base(args)
    gpu = Nvidia()
    report.update(sample_interval_seconds=args.interval, cpu_percent_scale="100% = one logical CPU",
                  logical_cpus=psutil.cpu_count(), nvidia_sampling_error=gpu.error,
                  process_names=[], peak_tree_rss_bytes=0)
    report["notes"] = ["RSS sums the process tree and can count shared pages more than once.",
                       "GPU/VRAM and system RAM include other programs; samples are not exact peaks.",
                       "Resource collection is a separate test process, excluded from the measured tree."]
    seen = {}
    psutil.cpu_percent()
    deadline = time.monotonic() + args.seconds
    while time.monotonic() < deadline:
        try:
            if not root.is_running() or root.create_time() != identity:
                break
            tree = [root] + root.children(recursive=True)
        except psutil.NoSuchProcess:
            break
        rss, cpu = 0, 0.0
        for process in tree:
            try:
                marker = (process.pid, process.create_time())
                if marker not in seen:
                    seen[marker] = process
                    name = process.name()
                    if name not in report["process_names"]:
                        report["process_names"].append(name)
                    process.cpu_percent()
                rss += process.memory_info().rss
                cpu += seen[marker].cpu_percent()
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                continue
        memory = psutil.virtual_memory()
        report["samples"].append({"at_utc": datetime.now(timezone.utc).isoformat(),
                                  "tree_cpu_percent": cpu, "tree_rss_bytes": rss,
                                  "system_cpu_percent": psutil.cpu_percent(),
                                  "system_ram_used_bytes": memory.total - memory.available,
                                  "nvidia": gpu.sample()})
        report["peak_tree_rss_bytes"] = max(report["peak_tree_rss_bytes"], rss)
        save(args.report, report)
        time.sleep(args.interval)
    report["native_tree_has_python"] = any("python" in n.lower() for n in report["process_names"])
    save(args.report, report)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--label", required=True)
    common.add_argument("--report", type=Path, required=True)
    sub = parser.add_subparsers(dest="command", required=True)
    expert = sub.add_parser("experts", parents=[common])
    expert.add_argument("--cluster-config", type=Path, required=True)
    expert.add_argument("--node", required=True)
    expert.add_argument("--pack", required=True)
    expert.add_argument("--native", required=True)
    api_parser = sub.add_parser("api", parents=[common])
    api_parser.add_argument("--port", type=int, required=True)
    api_parser.add_argument("--engine-log", type=Path)
    api_parser.add_argument("--max-tokens", type=int, default=128)
    for client in (expert, api_parser):
        client.add_argument("--warmup", type=int, default=2)
        client.add_argument("--repeats", type=int, default=20)
    sampler = sub.add_parser("sample", parents=[common])
    sampler.add_argument("--pid", type=int, required=True)
    sampler.add_argument("--interval", type=float, default=.5)
    sampler.add_argument("--seconds", type=float, default=1800)
    args = parser.parse_args()
    if args.command == "sample":
        if args.interval <= 0 or args.seconds <= 0:
            parser.error("interval and seconds must be positive")
        sample_process(args)
    else:
        if not 1 <= args.repeats <= 100 or not 1 <= args.warmup <= 20:
            parser.error("repeats must be 1..100 and warmup 1..20")
        if args.command == "experts":
            experts(args)
        else:
            api(args)


if __name__ == "__main__":
    main()
