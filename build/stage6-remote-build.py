import base64
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
REMOTE = "D:/Strata/stage6-native-build-20261011"
SSH = ["ssh", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes", "-o",
       "UserKnownHostsFile=" + str(ROOT / "tmp/stage6-known-hosts"), "-l",
       "missdeerme@hotmail.com", "192.168.233.126"]

def ps(script):
    encoded = base64.b64encode(("$ErrorActionPreference='Stop';"+script).encode("utf-16-le")).decode()
    result = subprocess.run(SSH + ["powershell -NoProfile -NonInteractive -EncodedCommand " + encoded],
                            capture_output=True, timeout=120, check=True)
    return result.stdout.decode("utf-8-sig", errors="replace").strip()

ps(f"if(Test-Path '{REMOTE}'){{throw 'build directory already exists'}}; New-Item -ItemType Directory '{REMOTE}'|Out-Null")
archive = ROOT / "tmp/stage6/source-host11.tar.gz"
manifest = {}
def filtered(info):
    if any(part in (".git", "__pycache__", ".pytest_cache") for part in Path(info.name).parts):
        return None
    return info
with tarfile.open(archive, "w:gz", compresslevel=3) as tar:
    # Current source, including the stage 6 compatibility fix; no Git/credentials/model data.
    tracked = subprocess.check_output(["git", "-c", "safe.directory="+ROOT.as_posix(), "ls-files"],cwd=ROOT,text=True).splitlines()
    roots = {"src", "include", "cmake", "tests", "tools", "serve", "configs", "third_party", "bench"}
    for name in tracked + ["tools/benchmark_native_runtime.py", "tools/validate_native_lan.py"]:
        path = ROOT / name
        if not path.is_file() or (Path(name).parts[0] not in roots and name not in ("CMakeLists.txt", "LICENSE")):
            continue
        if Path(name).parts[0] == "bench" and not name.startswith("bench/micro/"):
            continue
        tar.add(path, arcname="source/"+name, recursive=False)
        manifest[name] = hashlib.sha256(path.read_bytes()).hexdigest()
    for local, target in (
        (ROOT / "third_party/llama.cpp", "source/third_party/llama.cpp"),
        (ROOT / "tmp/native-image-deps/libwebp-1.6.0", "deps/libwebp-1.6.0"),
        (ROOT / "tmp/native-image-deps/libavif-1.4.2", "deps/libavif-1.4.2"),
        (ROOT / "tmp/native-image-deps/aom-3.14.1", "deps/aom-3.14.1"),
        (ROOT / "tmp/jsoncons-source/jsoncons-1.7.0", "deps/jsoncons-1.7.0"),
        (Path("D:/Tools/cmake-3.31.2-windows-x86_64/bin"), "dev/cmake/bin"),
        (Path("D:/Tools/cmake-3.31.2-windows-x86_64/share"), "dev/cmake/share"),
        (Path("D:/Tools/ninja.exe"), "dev/ninja.exe")):
        tar.add(local, arcname=target, filter=filtered)
(ROOT / "tmp/stage6/host11-source-hashes.json").write_text(json.dumps(manifest,indent=2),encoding="utf-8")
with archive.open("rb") as data:
    subprocess.run(SSH+["tar.exe -xf - -C "+REMOTE],stdin=data,check=True,timeout=600)
print("Copied current source + pinned offline dependencies and CMake/Ninja", archive.stat().st_size,flush=True)
batch = f'''@echo off
setlocal
cd /d {REMOTE.replace('/',chr(92))}
call "C:\\Program Files (x86)\\Intel\\oneAPI\\2026.1\\oneapi-vars.bat" > env.log 2>&1
if errorlevel 1 exit /b 1
set "PATH={REMOTE}/dev/cmake/bin;{REMOTE}/dev;%PATH%"
where icx-cl.exe > compiler-path.log 2>&1
cmake -S source -B build-native -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_CXX_COMPILER=C:/Program Files (x86)/Intel/oneAPI/compiler/2026.1/bin/icx-cl.exe" "-DCMAKE_C_COMPILER=C:/Program Files (x86)/Intel/oneAPI/compiler/2026.1/bin/icx-cl.exe" "-DCMAKE_CXX_FLAGS_RELEASE=/O3 /DNDEBUG /QxHost /fp:precise" "-DCMAKE_C_FLAGS_RELEASE=/O3 /DNDEBUG /QxHost /fp:precise" -DSTRATA_PORTABLE=OFF -DSTRATA_ENABLE_CUDA=OFF -DSTRATA_ENABLE_HIP=OFF -DSTRATA_BUILD_NATIVE_CLUSTER=ON -DSTRATA_BUILD_TESTS=ON -DSTRATA_GGML_DIR={REMOTE}/source/third_party/llama.cpp -DSTRATA_WEBP_DIR={REMOTE}/deps/libwebp-1.6.0 -DSTRATA_AVIF_DIR={REMOTE}/deps/libavif-1.4.2 -DSTRATA_AOM_DIR={REMOTE}/deps/aom-3.14.1 -DSTRATA_JSONCONS_DIR={REMOTE}/deps/jsoncons-1.7.0 > configure.log 2>&1
if errorlevel 1 exit /b 1
cmake --build build-native --target strata-host strata_network_backend strata-network-check strata-network-test-backend strata-cluster-test-role strata-server-check strata-text-check --parallel 6 > build.log 2>&1
set "result=%errorlevel%"
echo %result% > result.txt
exit /b %result%
'''
encoded = base64.b64encode(batch.encode()).decode()
result = ps(f"[IO.File]::WriteAllBytes('{REMOTE}/build-host11.cmd',[Convert]::FromBase64String('{encoded}')); "
            f"$p=Start-Process -WindowStyle Hidden cmd.exe -ArgumentList '/c','{REMOTE}/build-host11.cmd' -WorkingDirectory '{REMOTE}' -PassThru; "
            "@{pid=$p.Id;started=$p.StartTime.ToUniversalTime().Ticks.ToString()}|ConvertTo-Json -Compress")
(ROOT / "tmp/stage6/host11-build.json").write_text(json.dumps({"remote_root":REMOTE,"process":json.loads(result),"flags":"/O3 /DNDEBUG /QxHost /fp:precise","portable":False},indent=2),encoding="utf-8")
print(result,flush=True)
