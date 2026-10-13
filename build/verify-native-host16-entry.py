"""Check the new batch entry with the real native host, without loading a model."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[1]
entry=ROOT/'start-host16-native.bat'
OUT=ROOT/'tmp/rebase-20261012'
def run(args):
    p=subprocess.run(['cmd.exe','/d','/c','call',str(entry),*args],cwd=ROOT,capture_output=True,encoding='utf-8',errors='replace',timeout=300)
    assert p.returncode==0,(args,p.returncode,p.stdout,p.stderr)
    return p.stdout+p.stderr
help_text=run(['--help'])
assert 'Usage: start-host16-native.bat' in help_text
check=run(['--prefill','2048','--check'])
objects=[json.loads(line) for line in check.splitlines() if line.startswith('{')]
local_plan=next(o for o in objects if 'config_sha256' in o)
api=next(o for o in objects if o.get('valid') and o.get('native'))
args=api['engine_args']
prefills=[args[i+1] for i,a in enumerate(args[:-1]) if a=='--prefill']
assert prefills[-1]=='2048',prefills
assert '--network-workers' in args and '--serve' in args
spec=importlib.util.spec_from_file_location('remote_build',ROOT/'build/rebase-host11-build.py')
remote=importlib.util.module_from_spec(spec);spec.loader.exec_module(remote)
remote_plan=json.loads(remote.ps("& 'D:/Strata/stage6-native-build-20261011/build-native/strata-host.exe' cluster --config 'D:/Strata/cluster.json' --validate;exit $LASTEXITCODE"))
assert remote_plan==local_plan,(remote_plan,local_plan)
secret=(ROOT/'tmp/dual-host-test/network.key').read_text().strip()
assert secret not in check and secret not in help_text
(OUT/'native-entry-check.log').write_text(check,encoding='utf-8')
report={'entry':entry.name,'help_passed':True,'check_passed':True,'config_sha256':local_plan['config_sha256'],'host11_shared_plan_matches':True,'prefill_override':'2048','worker_key_not_printed':True,'model_or_service_started':False}
(OUT/'native-entry-verification.json').write_text(json.dumps(report,indent=2))
print(json.dumps(report,indent=2))
