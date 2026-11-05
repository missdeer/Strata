"""Run current host11 CPU and host16 CUDA builds together on private test ports."""
import base64
import copy
from datetime import datetime, timezone
import http.client
import importlib.util
import json
import os
from pathlib import Path
import re
import secrets
import socket
import subprocess
import time

import psutil

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/('tmp/rebase-20261012/lan-'+datetime.now(timezone.utc).strftime('%H%M%S'))
spec=importlib.util.spec_from_file_location('remote_build',ROOT/'build/rebase-host11-build.py')
remote=importlib.util.module_from_spec(spec)
spec.loader.exec_module(remote)
OUT.mkdir(exist_ok=False)
BUILD=ROOT/'build-intel'
REMOTE_BUILD=remote.REMOTE+'/build-native'
plan=json.loads((ROOT/'tmp/dual-host-native/cluster.json').read_text())
coord=next(n for n in plan['nodes'] if n['id']=='host16')
worker=next(n for n in plan['nodes'] if n['id']=='host11')
worker.update(port=18516,library=REMOTE_BUILD+'/strata_network_backend.dll',ram_cache_mib=1024,preload_experts=False)
coord.update(server_config=str(OUT/'server.json'),engine=str(BUILD/'strata.exe'),context=8192,prefill=512)
source=json.loads((ROOT/'tmp/dual-host-test/server.json').read_text())
source.update(exe=str(BUILD/'strata.exe'),port=18616,host='127.0.0.1',log=str(OUT/'engine.log'),open_browser=False,web_root=str(ROOT/'serve/web'))
source.pop('api_key',None)
source['lib_dirs']=[str(BUILD)]+source['lib_dirs']
source.get('env',{}).pop('STRATA_API_KEY',None)
(OUT/'server.json').write_text(json.dumps(source,indent=2))
(OUT/'cluster.json').write_text(json.dumps(plan,indent=2))
remote_config=remote.REMOTE+'/'+OUT.name+'-cluster.json'
remote.write_remote(OUT.name+'-cluster.json',json.dumps(plan,indent=2))
key=secrets.token_hex(32)
env=dict(os.environ,STRATA_NETWORK_KEY=key)
env.pop('STRATA_API_KEY',None)
env['PATH']=str(BUILD)+os.pathsep+env['PATH']
remote.ps(f"$s=New-Object Net.Sockets.TcpListener([Net.IPAddress]::Parse('192.168.233.126'),18516);$s.Start();$s.Stop();'Test worker port available'")
with socket.socket() as sock:
    sock.bind(('127.0.0.1',18616))
script=f"$env:STRATA_NETWORK_KEY=[Console]::In.ReadLine();$env:PATH='{REMOTE_BUILD};C:/Windows/System32';& '{REMOTE_BUILD}/strata-host.exe' cluster --config '{remote_config}' --node host11;exit $LASTEXITCODE"
encoded=base64.b64encode(script.encode('utf-16-le')).decode()
worker_log=(OUT/'worker-ssh.log').open('wb')
server_log=(OUT/'server.log').open('wb')
worker_process=None
server_process=None
report={'source_commit':'37baf30e83468584d9d35b24d9b790c8aac850b1','started_at_utc':datetime.now(timezone.utc).isoformat(),'worker_port':18516,'api_port':18616,'context':8192,'prefill':512,'worker_cache_mib':1024,'checks':{},'passed':False,'performance_claim':False}

def request(path,body=None):
    conn=http.client.HTTPConnection('127.0.0.1',18616,timeout=180)
    try:
        conn.request('POST' if body is not None else 'GET',path,json.dumps(body) if body is not None else None,{'Content-Type':'application/json'})
        response=conn.getresponse()
        data=response.read()
        if response.status!=200:
            raise AssertionError((response.status,data.decode(errors='replace')))
        return json.loads(data)
    finally:
        conn.close()

def stop_tree(process):
    if process is None or process.poll() is not None:
        return
    try:
        children=psutil.Process(process.pid).children(recursive=True)
    except psutil.NoSuchProcess:
        children=[]
    process.terminate()
    process.wait(timeout=20)
    for child in children:
        try: child.terminate()
        except psutil.NoSuchProcess: pass
    _,alive=psutil.wait_procs(children,timeout=10)
    for child in alive:
        child.kill()

try:
    worker_process=subprocess.Popen(remote.SSH+['powershell -NoProfile -NonInteractive -EncodedCommand '+encoded],stdin=subprocess.PIPE,stdout=worker_log,stderr=worker_log)
    worker_process.stdin.write((key+'\n').encode());worker_process.stdin.close()
    deadline=time.monotonic()+120
    while time.monotonic()<deadline:
        if worker_process.poll() is not None:
            raise AssertionError('Worker exited: '+(OUT/'worker-ssh.log').read_text(errors='replace')[-3000:])
        try:
            with socket.create_connection((worker['host'],worker['port']),timeout=1): pass
            break
        except OSError: time.sleep(.5)
    else: raise TimeoutError('Worker ready timeout')
    print('host11: new native worker listening',flush=True)
    server_process=subprocess.Popen([str(BUILD/'strata-host.exe'),'cluster','--config',str(OUT/'cluster.json'),'--node','host16'],cwd=OUT,env=env,stdout=server_log,stderr=server_log)
    deadline=time.monotonic()+300
    while time.monotonic()<deadline:
        if server_process.poll() is not None:
            raise AssertionError('Coordinator exited: '+(OUT/'server.log').read_text(errors='replace')[-4000:])
        try:
            if request('/health').get('loaded'): break
        except (OSError,AssertionError): pass
        time.sleep(.5)
    else: raise TimeoutError('Coordinator ready timeout')
    print('host16: CUDA coordinator ready',flush=True)
    base={'model':source['model_name']+'-network','temperature':0,'reasoning_effort':'none','max_tokens':32}
    turns=[]
    for name,prompt,needle in [('arithmetic','Reply with only the result: 2+2 =','4'),('remember','My code word is violet. Remember it. Reply OK.','OK'),('recall','What is my code word? Reply with just the word.','violet')]:
        messages=turns+[{'role':'user','content':prompt}] if name=='recall' else [{'role':'user','content':prompt}]
        result=request('/v1/chat/completions',{**base,'messages':messages})
        answer=result['choices'][0]['message']['content']
        assert needle.lower() in answer.lower(),(name,answer)
        report['checks'][name]={'answer':answer,'usage':result.get('usage'),'finish_reason':result['choices'][0]['finish_reason']}
        if name=='remember': turns=messages+[result['choices'][0]['message']]
        print(name+': PASS '+repr(answer),flush=True)
    metrics=request('/metrics')
    report['engine']=metrics.get('engine',{})
    log=(OUT/'logs/cluster-host16.log').read_text(encoding='utf-8',errors='replace')
    report['network_log_lines']=[line for line in log.splitlines() if 'network' in line.lower()][-25:]
    assert '192.168.233.126:18516' in log, 'Engine did not use test remote worker'
    assert re.search(r'network[^\n]*entries=[1-9]',log,re.I) or 'tasks' in (OUT/'worker-ssh.log').read_text(errors='replace'), 'No remote task evidence in logs'
    report['passed']=True
finally:
    stop_tree(server_process)
    if worker_process is not None:
        remote.ps(f"$expected='{REMOTE_BUILD}/strata-host.exe'.Replace('/','\\');$items=@(Get-CimInstance Win32_Process -Filter \"Name='strata-host.exe'\" | Where-Object {{$_.ExecutablePath -eq $expected -and $_.CommandLine.Contains('{OUT.name}-cluster.json')}});foreach($item in $items){{Stop-Process -Id $item.ProcessId -Force -ErrorAction SilentlyContinue}};exit 0")
    if worker_process is not None and worker_process.poll() is None:
        worker_process.terminate();worker_process.wait(timeout=20)
    worker_log.close();server_log.close()
    (OUT/'verification.json').write_text(json.dumps(report,ensure_ascii=False,indent=2))
print('LAN smoke: PASS',flush=True)
