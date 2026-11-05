"""Build and verify the current tree on host11 in a separate directory."""
import argparse
import base64
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'tmp/rebase-20261012'
REMOTE = 'D:/Strata/rebase-build-20261012-37baf30e'
OLD = 'D:/Strata/stage6-native-build-20261011'
SSH = ['ssh', '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=yes',
       '-o', 'ConnectTimeout=10', '-o', 'UserKnownHostsFile=' + str(ROOT / 'tmp/stage6-known-hosts'),
       '-l', 'missdeerme@hotmail.com', '192.168.233.126']

def ps(script, timeout=120):
    encoded = base64.b64encode(("$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue';[Console]::OutputEncoding=[Text.UTF8Encoding]::new();" + script).encode('utf-16-le')).decode()
    p = subprocess.run(SSH + ['powershell -NoProfile -NonInteractive -EncodedCommand ' + encoded], capture_output=True, timeout=timeout)
    output = p.stdout.decode('utf-8-sig', errors='replace') + p.stderr.decode('utf-8-sig', errors='replace')
    if p.returncode:
        raise RuntimeError(output)
    return output.strip()

def write_remote(name, content):
    payload = content.encode('utf-8')
    archive = io.BytesIO()
    with tarfile.open(fileobj=archive, mode='w') as tar:
        info = tarfile.TarInfo(name)
        info.size = len(payload)
        tar.addfile(info, io.BytesIO(payload))
    subprocess.run(SSH + ['tar.exe -xf - -C ' + REMOTE], input=archive.getvalue(), check=True, timeout=120)

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('action', choices=['build', 'status', 'verify', 'cleanup-lan', 'sync-fix', 'check-cleanup', 'inventory', 'inspect-runtime'])
    action = parser.parse_args().action
    OUT.mkdir(exist_ok=True)
    if action == 'inspect-runtime':
        code=f'''
import json,pathlib,sys
root=pathlib.Path('D:/Strata')
sys.path.insert(0,{REMOTE!r}+'/source/tools')
import build_cluster as B
cfg=json.loads((root/'cluster.json').read_text(encoding='utf-8-sig'))
worker=next(n for n in cfg['nodes'] if n['id']=='host11')
def closure(build,library):
 todo=[build/'strata-host.exe',library];files={{}};missing=[]
 candidates=B.index_dlls([build,library.parent])
 while todo:
  path=todo.pop()
  if str(path) in files:continue
  files[str(path)]=path.stat().st_size
  for name in B.pe_imports(path):
   lower=name.lower()
   if lower.startswith(('api-ms-win-','ext-ms-win-')) or lower in B.SYSTEM_DLLS:continue
   if lower not in candidates:missing.append(name)
   else:todo.append(candidates[lower])
 return {{'required_files':files,'missing_dependencies':missing,'bytes':sum(files.values())}}
old=root/'stage6-native-build-20261011/build-native'
new=root/'rebase-build-20261012-37baf30e/build-native'
print(json.dumps({{'current_worker':{{k:worker[k] for k in ('library','pack','native','workers','ram_cache_mib','preload_experts') if k in worker}},'current_native_runtime':closure(old,pathlib.Path(worker['library'])),'latest_native_runtime':closure(new,new/'strata_network_backend.dll')}},indent=2))
'''
        write_remote('inspect-runtime.py',code)
        result=ps(f"& 'H:/Python313/python.exe' '{REMOTE}/inspect-runtime.py';exit $LASTEXITCODE")
        (OUT/'host11-runtime-dependencies.json').write_text(result,encoding='utf-8')
        print(result)
    elif action == 'inventory':
        print(ps("""
$root='D:/Strata';
$entries=@(Get-ChildItem -LiteralPath $root -Force | ForEach-Object {
 if($_.PSIsContainer){$files=@(Get-ChildItem -LiteralPath $_.FullName -Recurse -File -ErrorAction SilentlyContinue);$bytes=($files|Measure-Object Length -Sum).Sum}else{$bytes=$_.Length};
 @{name=$_.Name;directory=$_.PSIsContainer;bytes=$bytes;children=if($_.PSIsContainer){@(Get-ChildItem -LiteralPath $_.FullName -Force | Select-Object -ExpandProperty Name)}else{@()}}
});
$scripts=@(Get-ChildItem -LiteralPath $root -File | Where-Object {$_.Extension -in '.bat','.cmd','.ps1'} | ForEach-Object {
 $lines=@([IO.File]::ReadAllLines($_.FullName) | ForEach-Object {if($_ -match '(?i)(STRATA_NETWORK_KEY|STRATA_API_KEY|password|token|secret)' -and $_ -match '='){ '[secret assignment redacted]' }else{[string]$_}});@{name=$_.Name;lines=$lines}
});
$active=@(Get-CimInstance Win32_Process -Filter "Name='strata-host.exe' OR Name='strata.exe' OR Name='python.exe'" | Select-Object ProcessId,ExecutablePath);
@{entries=$entries;scripts=$scripts;running=$active}|ConvertTo-Json -Depth 7 -Compress
""", timeout=180), flush=True)
    elif action == 'sync-fix':
        name='src/program/generate.cpp'
        manifest=json.loads((OUT/'host11-source-hashes.json').read_text())
        manifest[name]=hashlib.sha256((ROOT/name).read_bytes()).hexdigest()
        write_remote('source/'+name,(ROOT/name).read_text(encoding='utf-8'))
        # Hash the actual transported text, including its newline representation.
        digest=ps(f"(Get-FileHash '{REMOTE}/source/{name}' -Algorithm SHA256).Hash").lower()
        assert digest==manifest[name], (digest,manifest[name])
        write_remote('source-hashes.json',json.dumps(manifest))
        record=json.loads((OUT/'host11-verification.json').read_text())
        record['source_changes']={name:manifest[name]}
        record['source_change_build_effect']='generate.cpp is not part of the host11 CPU worker targets; their binaries are unchanged'
        write_remote('verification.json',json.dumps(record,indent=2))
        (OUT/'host11-verification.json').write_text(json.dumps(record,indent=2))
        (OUT/'host11-source-hashes.json').write_text(json.dumps(manifest,indent=2))
        print('Synchronized the engine-only compatibility fix; CPU worker binaries are unaffected.')
    elif action == 'check-cleanup':
        print(ps(f"$expected='{REMOTE}/build-native/strata-host.exe'.Replace('/','\\');$active=@(Get-CimInstance Win32_Process -Filter \"Name='strata-host.exe'\" | Where-Object {{$_.ExecutablePath -eq $expected}});if($active.Count){{throw 'Test processes remain'}};'No host11 test processes remain'"))
    elif action == 'cleanup-lan':
        print(ps(f"$expected='{REMOTE}/build-native/strata-host.exe'.Replace('/','\\');$items=@(Get-CimInstance Win32_Process -Filter \"Name='strata-host.exe'\" | Where-Object {{$_.ExecutablePath -eq $expected -and $_.CommandLine.Contains('lan-cluster.json')}});foreach($item in $items){{Stop-Process -Id $item.ProcessId -Force -ErrorAction SilentlyContinue}}; @{{stopped=@($items.ProcessId)}} | ConvertTo-Json -Compress"))
    elif action == 'status':
        print(ps(f"Get-Content '{REMOTE}/configure.log' -Tail 3 -ErrorAction SilentlyContinue; Get-Content '{REMOTE}/build.log' -Tail 8 -ErrorAction SilentlyContinue; if(Test-Path '{REMOTE}/result.txt'){{Get-Content '{REMOTE}/result.txt'}}; exit 0"), flush=True)
    elif action == 'build':
        commit = subprocess.check_output(['git', '-c', 'safe.directory='+ROOT.as_posix(), 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
        assert commit.startswith('37baf30e'), commit
        ps(f"if(Test-Path '{REMOTE}/build-host11.cmd'){{throw 'Build already prepared'}}; New-Item -ItemType Directory '{REMOTE}/source' -Force | Out-Null")
        names = subprocess.check_output(['git', '-c', 'safe.directory='+ROOT.as_posix(), 'ls-files'], cwd=ROOT, text=True).splitlines()
        manifest = {}
        archive = OUT / 'host11-source.tar.gz'
        with tarfile.open(archive, 'w:gz', compresslevel=3) as tar:
            for name in names:
                p = ROOT / name
                if not p.is_file() or (Path(name).parts[0] not in {'src','include','cmake','tools','tests','third_party','serve','configs','bench'} and name not in {'CMakeLists.txt','LICENSE'}):
                    continue
                if name.startswith('bench/') and not name.startswith('bench/micro/'):
                    continue
                tar.add(p, arcname=name, recursive=False)
                manifest[name] = hashlib.sha256(p.read_bytes()).hexdigest()
        (OUT/'host11-source-hashes.json').write_text(json.dumps(manifest, indent=2))
        with archive.open('rb') as stream:
            subprocess.run(SSH + ['tar.exe -xf - -C ' + REMOTE + '/source'], stdin=stream, check=True, timeout=120)
        write_remote('source-hashes.json', json.dumps(manifest))
        write_remote('source-commit.txt', commit)
        print(f'Synchronized {len(manifest)} source files ({archive.stat().st_size} bytes); commit {commit}', flush=True)
        batch = f'''@echo off
setlocal
cd /d {REMOTE.replace('/', chr(92))}
call "C:\\Program Files (x86)\\Intel\\oneAPI\\2026.1\\oneapi-vars.bat" > env.log 2>&1
if errorlevel 1 exit /b 1
set "PATH={OLD}/dev/cmake/bin;{OLD}/dev;%PATH%"
cmake -S source -B build-native -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_CXX_COMPILER=C:/Program Files (x86)/Intel/oneAPI/compiler/2026.1/bin/icx-cl.exe" "-DCMAKE_C_COMPILER=C:/Program Files (x86)/Intel/oneAPI/compiler/2026.1/bin/icx-cl.exe" "-DCMAKE_CXX_FLAGS_RELEASE=/O3 /DNDEBUG /QxHost /fp:precise" "-DCMAKE_C_FLAGS_RELEASE=/O3 /DNDEBUG /QxHost /fp:precise" -DSTRATA_PORTABLE=OFF -DSTRATA_ENABLE_CUDA=OFF -DSTRATA_ENABLE_HIP=OFF -DSTRATA_BUILD_NATIVE_CLUSTER=ON -DSTRATA_BUILD_TESTS=ON -DSTRATA_GGML_DIR={OLD}/source/third_party/llama.cpp -DSTRATA_WEBP_DIR={OLD}/deps/libwebp-1.6.0 -DSTRATA_AVIF_DIR={OLD}/deps/libavif-1.4.2 -DSTRATA_AOM_DIR={OLD}/deps/aom-3.14.1 -DSTRATA_JSONCONS_DIR={OLD}/deps/jsoncons-1.7.0 > configure.log 2>&1
if errorlevel 1 (
  type configure.log
  exit /b 1
)
cmake --build build-native --target strata-host strata_network_backend strata-network-check strata-network-test-backend strata-cluster-test-role strata-server-check strata-text-check --parallel 6 > build.log 2>&1
set "result=%errorlevel%"
echo %result% > result.txt
powershell -NoProfile -Command "Get-Content build.log -Tail 12"
exit /b %result%
'''
        write_remote('build-host11.cmd', batch.replace('\n','\r\n'))
        print(ps(f"& cmd.exe /d /c '{REMOTE}/build-host11.cmd'; exit $LASTEXITCODE", timeout=3600), flush=True)
    else:
        script = f'''
import hashlib,json,pathlib,subprocess,sys,os
root=pathlib.Path({REMOTE!r}); source=root/'source'; build=root/'build-native'
assert (root/'result.txt').read_text().strip()=='0'
manifest=json.loads((root/'source-hashes.json').read_text())
assert all(hashlib.sha256((source/n).read_bytes()).hexdigest()==h for n,h in manifest.items())
sys.path.insert(0,str(source/'tools'))
import build_cluster as B
cache=B.cache_values(build)
copied=B.copy_runtime(build,[*build.glob('*.exe'),build/'strata_network_backend.dll'],B.runtime_directories(cache,build,[]))
os.environ['PATH']=str(build)+os.pathsep+os.environ['PATH']
checks={{}}
commands={{
 'host-help':[str(build/'strata-host.exe'),'--help'],
 'native-host':[sys.executable,str(source/'tools/test_native_host.py'),'--exe',str(build/'strata-host.exe')],
 'network-validation':[sys.executable,str(source/'tools/test_network_validation.py'),'--library',str(build/'strata_network_backend.dll')],
 'native-cluster-real':[sys.executable,str(source/'tools/test_native_cluster.py'),'--worker',str(build/'strata-host.exe'),'--library',str(build/'strata_network_backend.dll'),'--pack','D:/Strata-data/packs/iq3_s','--native','D:/Strata-data/models/IQ3_S/Qwen3.8-Flash-Next-GSQ-RCO-IQ3_S-00001-of-00002.gguf']
}}
for name,cmd in commands.items():
 with (root/(name+'.log')).open('wb') as log:
  result=subprocess.run(cmd,cwd=source,stdout=log,stderr=log,timeout=600)
 checks[name]={{'exit_code':result.returncode,'log':str(root/(name+'.log'))}}
 print(name+': '+str(result.returncode),flush=True)
 print((root/(name+'.log')).read_text(encoding='utf-8',errors='replace')[-1800:],flush=True)
record={{'source_commit':(root/'source-commit.txt').read_text(),'source_files_verified':len(manifest),'build_directory':str(build),'compiler':cache['CMAKE_CXX_COMPILER'],'flags':cache['CMAKE_CXX_FLAGS_RELEASE'],'copied_runtime':copied,'checks':checks,'artifacts':{{n:hashlib.sha256((build/n).read_bytes()).hexdigest() for n in ['strata-host.exe','strata_network_backend.dll']}}}}
(root/'verification.json').write_text(json.dumps(record,indent=2))
assert all(c['exit_code']==0 for c in checks.values()),checks
'''
        write_remote('verify.py', script)
        print(ps(f"& 'H:/Python313/python.exe' -u '{REMOTE}/verify.py'; exit $LASTEXITCODE", timeout=1800), flush=True)
        data=ps(f"[Convert]::ToBase64String([IO.File]::ReadAllBytes('{REMOTE}/verification.json'))")
        (OUT/'host11-verification.json').write_bytes(base64.b64decode(data))
