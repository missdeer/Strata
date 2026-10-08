import os
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tools"))
from cluster_config import load_cluster

plan = load_cluster(root / "tmp/dual-host-test/cluster.json")
env = dict(os.environ, STRATA_NETWORK_KEY=(root / "tmp/dual-host-test/network.key").read_text().strip(),
           STRATA_NETWORK_ASSIGNMENT=plan.assignment, STRATA_NETWORK_CONFIG_ID=plan.digest)
env["PATH"] = str(root / "dist/cluster/host16") + os.pathsep + env.get("PATH", "")
command = [str(root / "dist/cluster/host16/strata-network-check.exe"),
           "D:/Projects/Strata-data/packs/iq3_s", plan.endpoints, "48", "-",
           "D:/Projects/Strata-data/models/IQ3_S/Qwen3.8-Flash-Next-GSQ-RCO-IQ3_S-00001-of-00002.gguf"]
result = subprocess.run(command, env=env, text=True, capture_output=True, timeout=40)
print(result.stdout, end="")
print(result.stderr, end="")
print("LAN expert parity:", "PASS" if result.returncode == 0 else "FAIL", result.returncode)
raise SystemExit(result.returncode)
