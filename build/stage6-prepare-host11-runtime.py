import base64
import io
from pathlib import Path
import subprocess
import tarfile

root = "D:/Strata/stage6-native-build-20261011"
ssh = ["ssh", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes", "-o",
       "UserKnownHostsFile=D:/Projects/Strata/tmp/stage6-known-hosts", "-l",
       "missdeerme@hotmail.com", "192.168.233.126"]
local_root = Path(__file__).resolve().parents[1]
archive = io.BytesIO()
with tarfile.open(fileobj=archive, mode='w') as tar:
    for name in ("src/runtime/expert_store.cpp", "src/runtime/server_recovery.cpp", "src/runtime/engine_channel.cpp",
                 "src/runtime/server_case_properties.hpp", "tools/test_native_cluster.py", "tools/test_native_server.py"):
        tar.add(local_root/name, arcname=name)
subprocess.run(ssh+["tar.exe -xf - -C "+root+'/source'],input=archive.getvalue(),check=True,capture_output=True)
batch = f'''@echo off
setlocal
call "C:\\Program Files (x86)\\Intel\\oneAPI\\2026.1\\oneapi-vars.bat" > {root}/incremental-env.log 2>&1
if errorlevel 1 exit /b 1
{root}/dev/cmake/bin/cmake.exe --build {root}/build-native --target strata-host strata_network_backend strata-network-check strata-network-test-backend strata-cluster-test-role strata-server-check strata-text-check --parallel 6 > {root}/incremental.log 2>&1
exit /b %errorlevel%
'''
data = base64.b64encode(batch.encode()).decode()
ps = f"[IO.File]::WriteAllBytes('{root}/incremental.cmd',[Convert]::FromBase64String('{data}')); & cmd.exe /c '{root}/incremental.cmd'; exit $LASTEXITCODE"
encoded = base64.b64encode(ps.encode('utf-16-le')).decode()
subprocess.run(ssh+["powershell -NoProfile -NonInteractive -EncodedCommand "+encoded],check=True,capture_output=True)
script = f'''
import json, pathlib, subprocess, sys
root = pathlib.Path({root!r})
sys.path.insert(0, str(root/'source/tools'))
import build_cluster as B
build = root/'build-native'
cache = B.cache_values(build)
print('host imports:',B.pe_imports(build/'strata-host.exe'),flush=True)
print('backend imports:',B.pe_imports(build/'strata_network_backend.dll'),flush=True)
copied = B.copy_runtime(build, [*build.glob('*.exe'),build/'strata_network_backend.dll'],B.runtime_directories(cache,build,[]))
print('collected runtime:',copied,flush=True)
(root/'runtime-collection.json').write_text(json.dumps({{'copied':copied,'compiler':cache['CMAKE_CXX_COMPILER'],'flags':cache['CMAKE_CXX_FLAGS_RELEASE']}},indent=2))
command=[sys.executable,str(root/'source/tools/test_native_cluster.py'),'--worker',str(build/'strata-host.exe'),'--library',str(build/'strata_network_backend.dll'),'--pack','D:/Strata-data/packs/iq3_s','--native','D:/Strata-data/models/IQ3_S/Qwen3.8-Flash-Next-GSQ-RCO-IQ3_S-00001-of-00002.gguf']
with (root/'worker-parity.log').open('wb') as log:
    result=subprocess.run(command,stdout=log,stderr=log)
print((root/'worker-parity.log').read_text(errors='replace'),flush=True)
raise SystemExit(result.returncode)
'''
payload=base64.b64encode(script.encode()).decode()
ps=f"[IO.File]::WriteAllBytes('{root}/prepare-runtime.py',[Convert]::FromBase64String('{payload}')); & 'H:/Python313/python.exe' '{root}/prepare-runtime.py'; exit $LASTEXITCODE"
encoded=base64.b64encode(ps.encode('utf-16-le')).decode()
result=subprocess.run(ssh+["powershell -NoProfile -NonInteractive -EncodedCommand "+encoded],capture_output=True)
print(result.stdout.decode('utf-8-sig',errors='replace'))
print(result.stderr.decode('utf-8-sig',errors='replace'))
raise SystemExit(result.returncode)
