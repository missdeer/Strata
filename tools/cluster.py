"""Read one shared startup plan and run this machine's configured Strata role."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from cluster_config import load_cluster
from make_network_config import make_config

ROOT = Path(__file__).resolve().parents[1]


def run(args):
    # Reuse the server's Windows job containment so closing this launcher also ends its role process.
    sys.path.insert(0, str(ROOT))
    from serve.winjob import contain
    process = subprocess.Popen(args, cwd=ROOT)
    contain(process)
    try:
        return process.wait()
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait(timeout=5)


def command(plan, node_id):
    node = plan.node(node_id)
    if node["role"] == "expert-worker":
        return [sys.executable, str(ROOT / "tools/network_worker.py"), "--cluster-config", str(plan.path), "--node", node_id]
    return [sys.executable, "-m", "serve.server", "--engine", "strata"]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--config", required=True, help="the identical cluster JSON copied to every node")
    p.add_argument("--node", help="this machine's stable node id; required except with --validate")
    p.add_argument("--validate", action="store_true", help="validate and print the static plan without starting a process")
    p.add_argument("--no-api-key", action="store_true", help="disable coordinator API authentication on 127.0.0.1")
    p.add_argument("--prefill", choices=("512", "1024", "2048", "4096", "8192", "auto", "auto:8192"),
                   help="local coordinator prompt chunk override; does not change the shared worker plan")
    a = p.parse_args()
    try:
        plan = load_cluster(a.config)
        plan.path = Path(a.config).resolve()
        node = plan.node(a.node) if a.node else None
        if not a.validate and node is None:
            p.error("--node is required for startup")
        print(f"cluster={plan.data['cluster_id']} revision={plan.data['revision']} config_sha256={plan.digest}", flush=True)
        print(f"coordinator={plan.coordinator['id']} workers={plan.endpoints}", flush=True)
        for worker in plan.workers:
            print(f"{worker['id']}: {worker['backend']} expert_ranges={worker['expert_ranges']}", flush=True)
        if a.validate:
            return
        key = os.environ.get("STRATA_NETWORK_KEY", "").encode("utf-8")
        if not 16 <= len(key) <= 256:
            p.error("set STRATA_NETWORK_KEY to the same 16..256 byte secret on every node")
        args = command(plan, a.node)
        if node["role"] == "expert-worker":
            if a.prefill:
                p.error("--prefill applies only to the coordinator")
            raise SystemExit(run(args))
        source = json.loads(Path(node["server_config"]).read_text(encoding="utf-8"))
        cfg = make_config(source, plan.endpoints, node["context"], node["engine"],
                          node.get("network_reduce", False), node.get("network_local_ram_mib", 0))
        if a.no_api_key:
            cfg["host"] = "127.0.0.1"
            cfg.pop("api_key", None)
            os.environ.pop("STRATA_API_KEY", None)
        cfg["log"] = str(ROOT / "logs" / f"cluster-{a.node}.log")
        (ROOT / "logs").mkdir(exist_ok=True)
        cfg["args"] += ["--prefill", a.prefill or str(node["prefill"]), "--network-assignment", plan.assignment,
                        "--network-config-id", plan.digest, "--network-timeout-ms", str(plan.data["scheduler"]["timeout_ms"])]
        # Runtime config is private to this invocation. The installed config and shared plan are never rewritten.
        with tempfile.TemporaryDirectory(prefix="strata-cluster-") as folder:
            runtime = Path(folder) / "coordinator.json"
            runtime.write_text(json.dumps(cfg, ensure_ascii=False, indent=2), encoding="utf-8")
            # serve.server's CLI port default does not read the config's port; pass it explicitly.
            port = cfg.get("port", 8080)
            if type(port) is not int or not 1 <= port <= 65535:
                raise ValueError("coordinator server config port must be an integer in 1..65535")
            raise SystemExit(run(args + ["--config", str(runtime), "--port", str(port)]))
    except (ValueError, OSError, KeyError) as error:
        p.error(str(error))


if __name__ == "__main__":
    main()
