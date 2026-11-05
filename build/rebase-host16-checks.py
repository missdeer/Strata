"""Verify freshly built CUDA/native artifacts and the real GPU expert path."""
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'tmp/rebase-20261012'
BUILD=ROOT/'build-intel'
env=dict(os.environ)
env['PATH']=str(BUILD)+os.pathsep+env['PATH']
pack='D:/Projects/Strata-data/packs/iq3_s'
native='D:/Projects/Strata-data/models/IQ3_S/Qwen3.8-Flash-Next-GSQ-RCO-IQ3_S-00001-of-00002.gguf'
checks={
 'host-help':[str(BUILD/'strata-host.exe'),'--help'],
 'engine-help':[str(BUILD/'strata.exe'),'--help'],
 'gpu-devices':[str(BUILD/'strata-device.exe'),'--list-devices'],
 'gpu-selftest':[str(BUILD/'strata-device.exe'),'--selftest'],
 'native-host':[sys.executable,'tools/test_native_host.py','--exe',str(BUILD/'strata-host.exe')],
 'network-validation':[sys.executable,'tools/test_network_validation.py','--library',str(BUILD/'strata_network_backend.dll')],
 'native-cluster-gpu-real':[sys.executable,'tools/test_native_cluster.py','--worker',str(BUILD/'strata-host.exe'),'--library',str(BUILD/'strata_network_backend.dll'),'--pack',pack,'--native',native,'--gpu-device','0'],
}
report={'source_commit':subprocess.check_output(['git','-c','safe.directory='+ROOT.as_posix(),'rev-parse','HEAD'],cwd=ROOT,text=True).strip(),'checked_at_utc':datetime.now(timezone.utc).isoformat(),'backend':'CUDA sm_89','checks':{},'artifacts':{n:hashlib.sha256((BUILD/n).read_bytes()).hexdigest() for n in ['strata.exe','strata-host.exe','strata_network_backend.dll']}}
for name,cmd in checks.items():
    path=OUT/('host16-'+name+'.log')
    with path.open('wb') as log:
        result=subprocess.run(cmd,cwd=ROOT,env=env,stdout=log,stderr=log,timeout=600)
    report['checks'][name]={'exit_code':result.returncode,'log':path.relative_to(ROOT).as_posix()}
    print(name+': '+('PASS' if result.returncode==0 else 'FAIL'),flush=True)
    print(path.read_text(encoding='utf-8',errors='replace')[-1600:],flush=True)
    (OUT/'host16-verification.json').write_text(json.dumps(report,indent=2))
if any(c['exit_code'] for c in report['checks'].values()):
    raise SystemExit(1)
