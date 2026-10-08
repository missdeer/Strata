from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
key = (root / "tmp/dual-host-test/network.key").read_text().strip()
result = subprocess.run(["cmd.exe", "/d", "/c", "call", str(root / "start-host16.bat"), "--check"],
                        cwd=root, capture_output=True, text=True, timeout=20)
print(result.stdout.replace(key, "[read from network.key]"), end="")
print(result.stderr.replace(key, "[read from network.key]"), end="")
print("Launcher exit:", result.returncode)
raise SystemExit(result.returncode)
