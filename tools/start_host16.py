"""Start the tested host16/host11 cluster, or show agent settings for its running OpenAI API."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import urllib.error
import urllib.request

from cluster_config import load_cluster
from make_network_config import make_config

ROOT = Path(__file__).resolve().parents[1]


def running_api(base, key, model, context):
    headers = {"Authorization": "Bearer " + key}
    # Local service checks must not follow a system HTTP proxy or redirect authentication elsewhere.
    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, request, file, code, message, headers, new_url):
            return None
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())
    try:
        request = urllib.request.Request(base + "/health", headers=headers)
        with opener.open(request, timeout=3) as response:
            health = json.load(response)
    except urllib.error.HTTPError as error:
        raise ValueError(f"port is occupied by an API that returned HTTP {error.code}") from None
    except urllib.error.URLError as error:
        if isinstance(error.reason, ConnectionRefusedError):
            return False
        # A timeout or an unexpected protocol response must not start another engine on the same port.
        raise ValueError(f"cannot check the local API: {error.reason}") from None
    if (health.get("service") != "strata" or health.get("model") != model
            or health.get("max_context") != context or not health.get("api_key")):
        raise ValueError("another service/configuration is using the API port; stop it before starting this cluster")
    try:
        request = urllib.request.Request(base + "/v1/models", headers=headers)
        with opener.open(request, timeout=3) as response:
            models = json.load(response)
    except urllib.error.HTTPError as error:
        raise ValueError(f"existing API authentication failed (HTTP {error.code}); verify the API key") from None
    if not health.get("loaded") or model not in [item.get("id") for item in models.get("data", [])]:
        raise ValueError("the API is running but its model is not ready; wait for startup to finish")
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=ROOT / "tmp/dual-host-test/cluster.json")
    parser.add_argument("--key-file", type=Path, default=ROOT / "tmp/dual-host-test/network.key")
    parser.add_argument("--check", action="store_true", help="show settings and check readiness without starting a process")
    args = parser.parse_args()
    try:
        plan = load_cluster(args.config)
        node = plan.node("host16")
        if node["role"] != "coordinator" or len(plan.workers) != 1 or plan.workers[0]["id"] != "host11":
            raise ValueError("this launcher requires the host16 + host11 two-host plan")
        key = args.key_file.read_text(encoding="ascii").strip()
        if not 16 <= len(key.encode("utf-8")) <= 256:
            raise ValueError("the shared network key must contain 16..256 bytes")
        source = json.loads(Path(node["server_config"]).read_text(encoding="utf-8-sig"))
        cfg = make_config(source, plan.endpoints, node["context"], node["engine"])
        if not Path(cfg["exe"]).is_file():
            raise ValueError(f"coordinator binary is missing: {cfg['exe']}")
        port = cfg.get("port", 8080)
        if type(port) is not int or not 1 <= port <= 65535:
            raise ValueError("invalid API port in the coordinator server configuration")
        base = f"http://127.0.0.1:{port}"
        # Keep the API key consistent with the tested service and the existing host11 worker key.
        env = dict(os.environ, STRATA_NETWORK_KEY=key, STRATA_API_KEY=key)
        print("OpenAI-compatible API / agent connection settings", flush=True)
        print(f"  Base URL: {base}/v1", flush=True)
        print(f"  Model:    {cfg['model_name']}", flush=True)
        print(f"  API key:  {key}", flush=True)
        print(f"  Context:  {node['context']} tokens", flush=True)
        print("  Client request timeout: 300 seconds or longer", flush=True)
        print(f"  Chat UI:  {base}", flush=True)
        print(f"  Worker:   {plan.endpoints}", flush=True)
        if running_api(base, key, cfg["model_name"], node["context"]):
            print("The authenticated API is already ready. Connect your agent; no second engine was started.", flush=True)
            return 0
        worker = plan.workers[0]
        try:
            with socket.create_connection((worker["host"], worker["port"]), timeout=3):
                pass
        except OSError:
            raise ValueError("host11 worker is not reachable. Run D:\\Strata\\start-host11.bat on host11 first.") from None
        if args.check:
            print("Host11 worker is reachable; the host16 API is stopped. Run start-host16.bat to start it.", flush=True)
            return 0
        print("Starting host16. Keep this window open; Ctrl+C stops the coordinator.", flush=True)
        return subprocess.call([sys.executable, "-u", str(ROOT / "tools/cluster.py"), "--config", str(args.config.resolve()),
                                "--node", "host16"], cwd=ROOT, env=env)
    except (ValueError, OSError, KeyError) as error:
        print(f"Startup failed: {error}", file=sys.stderr, flush=True)
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
