import json
from pathlib import Path
import secrets
import sys

root = Path(__file__).resolve().parents[1]
folder = root / "tmp/dual-host-test"
folder.mkdir(parents=True, exist_ok=True)
key_path = folder / "network.key"
if not key_path.exists():
    key_path.write_text(secrets.token_hex(32) + "\n", encoding="ascii")
server = json.loads((root / "strata-iq3_s.json").read_text(encoding="utf-8"))
server.update(port=8080, host="127.0.0.1", open_browser=False, model_name="qwen3.8-flash-next-iq3_s")
server["args"] += ["--prompt-cache", "0", "--adapt-every", "0"]
server_path = folder / "server.json"
server_path.write_text(json.dumps(server, indent=2), encoding="utf-8")
plan = json.loads((root / "configs/cluster-2hosts.json").read_text(encoding="utf-8"))
plan["cluster_id"] = "strata-iq3-s-dual-host-test"
plan["revision"] = 4
plan["scheduler"]["timeout_ms"] = 5000
plan["nodes"] = [node for node in plan["nodes"] if node["online"]]
main, worker = plan["nodes"]
main.update(host="192.168.233.136", context=131072, server_config=str(server_path),
            engine=str(root / "dist/cluster/host16/strata.exe"))
worker.update(host="192.168.233.126", library="D:/Strata/strata_network_backend.dll", ram_cache_mib=49152, preload_experts=True)
(folder / "cluster.json").write_text(json.dumps(plan, indent=2), encoding="utf-8")
batch = r"""@echo off
setlocal
cd /d "%~dp0"
if not exist "network.key" (echo Missing network.key & pause & exit /b 1)
set /p STRATA_NETWORK_KEY=<"network.key"
where python.exe >nul 2>&1
if errorlevel 1 (
  py -3 tools\network_worker.py --cluster-config cluster.json --node host11
) else (
  python tools\network_worker.py --cluster-config cluster.json --node host11
)
echo.
echo Worker exited. Keep this window open while the worker is running.
pause
"""
(folder / "start-host11.bat").write_bytes(batch.replace("\n", "\r\n").encode("ascii"))
sys.path.insert(0, str(root / "tools"))
from cluster_config import load_cluster
config = load_cluster(folder / "cluster.json")
print(f"Prepared dual-host test: {config.endpoints}; config_sha256={config.digest}")
