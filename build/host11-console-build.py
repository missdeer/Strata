"""Update and compile the native host on host11 with its installed oneAPI."""
import argparse
import base64
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
REMOTE = "D:/Strata/stage6-native-build-20261011"
SSH = ["ssh", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes",
       "-o", "ConnectTimeout=10", "-o", "UserKnownHostsFile=" + str(ROOT / "tmp/stage6-known-hosts"),
       "-l", "missdeerme@hotmail.com", "192.168.233.126"]

def ps(script, timeout=60):
    text = "$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue';[Console]::OutputEncoding=[Text.UTF8Encoding]::new();" + script
    encoded = base64.b64encode(text.encode("utf-16-le")).decode()
    result = subprocess.run(SSH + ["powershell -NoProfile -NonInteractive -EncodedCommand " + encoded],
                            capture_output=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(result.stdout.decode("utf-8", errors="replace") + result.stderr.decode("utf-8", errors="replace"))
    return result.stdout.decode("utf-8-sig", errors="replace").strip()

parser = argparse.ArgumentParser()
parser.add_argument("action", choices=("probe", "build", "run", "status", "verify", "test"))
args = parser.parse_args()
if args.action == "probe":
    print(ps(f"""
$active=@(Get-Process -Name strata-host,strata -ErrorAction SilentlyContinue | Select-Object Id,Path);
$cache=@(Get-Content '{REMOTE}/build-native/CMakeCache.txt' | Select-String '^(CMAKE_CXX_COMPILER:|CMAKE_CXX_FLAGS_RELEASE:|CMAKE_C_FLAGS_RELEASE:|STRATA_PORTABLE:)');
@{{running=$active;cache=@($cache | ForEach-Object {{$_.Line}});host=(Get-Item '{REMOTE}/build-native/strata-host.exe').LastWriteTimeUtc.ToString('o')}} | ConvertTo-Json -Depth 4 -Compress
"""))
elif args.action == "build":
    # Refuse to alter the active target. A separate build can be selected if needed.
    print(ps(f"""
$active=@(Get-Process -Name strata-host -ErrorAction SilentlyContinue | Where-Object {{$_.Path -eq '{REMOTE}/build-native/strata-host.exe'.Replace('/','\')}});
if($active.Count){{throw 'Target worker is running; leave it intact and use a separate build directory'}};
$backup='{REMOTE}/console-backup';New-Item -ItemType Directory -Path $backup -Force | Out-Null;
foreach($name in @('strata-host.exe','strata_network_backend.dll')){{if(-not(Test-Path "$backup/$name")){{Copy-Item -LiteralPath (Join-Path '{REMOTE}/build-native' $name) -Destination "$backup/$name"}}}};
'Original target binaries backed up.'
"""))
    archive = io.BytesIO()
    manifest = {}
    names = ("src/runtime/engine_process.cpp", "tests/runtime/server_test_engine.cpp",
             "tools/test_native_server.py", "docs/MULTI_HOST.md")
    with tarfile.open(fileobj=archive, mode="w") as tar:
        for name in names:
            tar.add(ROOT / name, arcname=name)
            manifest[name] = hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
    subprocess.run(SSH + ["tar.exe -xf - -C " + REMOTE + "/source"],
                   input=archive.getvalue(), capture_output=True, check=True, timeout=60)
    batch = f'''@echo off
setlocal
cd /d {REMOTE.replace('/', chr(92))}
call "C:\\Program Files (x86)\\Intel\\oneAPI\\2026.1\\oneapi-vars.bat" > console-env.log 2>&1
if errorlevel 1 (
  echo 1 > console-result.txt
  exit /b 1
)
"{REMOTE}/dev/cmake/bin/cmake.exe" --build build-native --target strata-host strata_network_backend strata-network-check strata-server-check strata-server-test-engine --parallel 6 > console-build.log 2>&1
set "console_result=%errorlevel%"
echo %console_result% > console-result.txt
exit /b %console_result%
'''
    encoded = base64.b64encode(batch.replace("\n", "\r\n").encode()).decode()
    batch_path = REMOTE.replace('/', chr(92)) + "\\build-console-host11.cmd"
    print(ps(f"""
if(Test-Path '{REMOTE}/console-result.txt'){{throw 'Console build result already exists'}};
[IO.File]::WriteAllBytes('{REMOTE}/build-console-host11.cmd',[Convert]::FromBase64String('{encoded}'));
& cmd.exe /d /c '{batch_path}'; exit $LASTEXITCODE
""", timeout=600), flush=True)
    (ROOT / "tmp/stage6/host11-console-source-hashes.json").write_text(json.dumps(manifest, indent=2))
elif args.action == "run":
    batch_path = REMOTE.replace('/', chr(92)) + "\\build-console-host11.cmd"
    print(ps(f"& cmd.exe /d /c '{batch_path}'; exit $LASTEXITCODE", timeout=600), flush=True)
elif args.action == "status":
    print(ps(f"if(Test-Path '{REMOTE}/console-result.txt'){{Get-Content '{REMOTE}/console-result.txt'}}else{{'NO_RESULT'}}; if(Test-Path '{REMOTE}/console-build.log'){{Get-Content '{REMOTE}/console-build.log' -Tail 12}}else{{Get-Content '{REMOTE}/console-env.log' -Tail 8 -ErrorAction SilentlyContinue; @(Get-Process -Name cmd,icx-cl,ninja -ErrorAction SilentlyContinue) | Select-Object Id,CPU,StartTime | ConvertTo-Json -Compress}}; Write-Output 'STATUS_END'"))
elif args.action == "verify":
    verified = ps(f"""
if((Get-Content '{REMOTE}/console-result.txt').Trim() -ne '0'){{throw 'Remote build did not succeed'}};
& '{REMOTE}/build-native/strata-host.exe' --help;
if($LASTEXITCODE){{throw 'Native host help failed'}};
$hash=(Get-FileHash -Algorithm SHA256 '{REMOTE}/source/src/runtime/engine_process.cpp').Hash;
$binary=Get-Item '{REMOTE}/build-native/strata-host.exe';
@{{source_sha256=$hash;binary_sha256=(Get-FileHash -Algorithm SHA256 $binary.FullName).Hash;path=$binary.FullName;updated=$binary.LastWriteTimeUtc.ToString('o');flags=@(Get-Content '{REMOTE}/build-native/CMakeCache.txt' | Select-String '^(CMAKE_CXX_COMPILER:|CMAKE_CXX_FLAGS_RELEASE:|STRATA_PORTABLE:)' | ForEach-Object {{$_.Line}})}} | ConvertTo-Json -Compress
""")
    record = json.loads(verified.splitlines()[-1])
    expected = hashlib.sha256((ROOT / "src/runtime/engine_process.cpp").read_bytes()).hexdigest()
    if record["source_sha256"].lower() != expected:
        raise RuntimeError("host11 source differs from local source")
    (ROOT / "tmp/stage6/host11-console-build-verification.json").write_text(json.dumps(record, indent=2))
    print(verified)
else:
    print(ps(f"""
New-Item -ItemType Directory '{REMOTE}/source/tmp' -Force | Out-Null;
$ErrorActionPreference='Continue';
& 'H:/Python313/python.exe' -u '{REMOTE}/source/tools/test_native_server.py' --exe '{REMOTE}/build-native/strata-host.exe' --engine '{REMOTE}/build-native/strata-server-test-engine.exe' --only test_engine_console_mirrors_only_new_stderr_and_preserves_log --report '{REMOTE}/console-http.json' > '{REMOTE}/console-http.log' 2>&1;
$httpCode=$LASTEXITCODE;
Get-Content '{REMOTE}/console-http.log' -Tail 12;
& 'H:/Python313/python.exe' -u '{REMOTE}/source/tools/test_native_cluster.py' --worker '{REMOTE}/build-native/strata-host.exe' --library '{REMOTE}/build-native/strata_network_backend.dll' --pack 'D:/Strata-data/packs/iq3_s' --native 'D:/Strata-data/models/IQ3_S/Qwen3.8-Flash-Next-GSQ-RCO-IQ3_S-00001-of-00002.gguf' > '{REMOTE}/console-worker.log' 2>&1;
$workerCode=$LASTEXITCODE;
Get-Content '{REMOTE}/console-worker.log' -Tail 24;
@{{console_http_exit=$httpCode;worker_exit=$workerCode}} | ConvertTo-Json -Compress;
if($httpCode -or $workerCode){{exit 1}}
""", timeout=600), flush=True)
