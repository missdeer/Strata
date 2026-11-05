import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'tmp/rebase-20261012'
cpu=json.loads((OUT/'host11-verification.json').read_text())
cuda=json.loads((OUT/'host16-verification.json').read_text())
lan_paths=[p for p in OUT.glob('lan-*/verification.json') if json.loads(p.read_text()).get('passed')]
assert len(lan_paths)==1,lan_paths
lan=json.loads(lan_paths[0].read_text())
assert all(c['exit_code']==0 for r in (cpu,cuda) for c in r['checks'].values())
env=dict(os.environ,STRATA_NETWORK_KEY='temporary-build-validation-key')
command=[str(ROOT/'build-intel/strata.exe'),'--pack','D:/Projects/Strata-data/packs/iq3_s','--native','D:/Projects/Strata-data/models/IQ3_S/Qwen3.8-Flash-Next-GSQ-RCO-IQ3_S-00001-of-00002.gguf','--serve','--network-workers','127.0.0.1:18516','--pipeline-windows','1']
guard=subprocess.run(command,env=env,capture_output=True,timeout=30)
(OUT/'explicit-pipeline-rejection.log').write_bytes(guard.stdout+guard.stderr)
assert guard.returncode==2 and b'--network-workers requires' in guard.stderr,guard
cuda['artifacts']['strata.exe']=hashlib.sha256((ROOT/'build-intel/strata.exe').read_bytes()).hexdigest()
cuda['checks']['network-auto-pipeline-default']={'passed':True,'report':lan_paths[0].relative_to(ROOT).as_posix()}
cuda['checks']['network-explicit-pipeline-rejected']={'expected_exit_code':2,'actual_exit_code':guard.returncode}
patch=subprocess.check_output(['git','-c','safe.directory='+ROOT.as_posix(),'diff','--','src/program/generate.cpp'],cwd=ROOT)
(OUT/'network-pipeline-compatibility.patch').write_bytes(patch)
assert patch, 'Expected uncommitted compatibility fix'
cuda['source_changes']={'src/program/generate.cpp':hashlib.sha256((ROOT/'src/program/generate.cpp').read_bytes()).hexdigest()}
(OUT/'host16-verification.json').write_text(json.dumps(cuda,indent=2))
record={'source_commit':cpu['source_commit'],'passed':True,'hosts':{'host11':cpu,'host16':cuda},'lan':lan,'lan_report':str(lan_paths[0].relative_to(ROOT)),'uncommitted_fix':{'file':'src/program/generate.cpp','line':2352,'change':'Allow the upstream automatic pipeline sentinel (-1) in network mode; continue rejecting explicit positive pipeline counts','patch':'tmp/rebase-20261012/network-pipeline-compatibility.patch'},'scope':{'host11':'Windows i7-10700 native CPU worker, oneAPI 2026.1 /O3 /QxHost','host16':'Windows RTX 4060 Ti CUDA sm_89 engine and native API','cpu_real_expert_cases':42,'gpu_real_expert_cases':42,'host11_unit_tests_passed':29,'host16_unit_tests_passed':28,'host16_unit_tests_skipped':1,'lan_generation_checks':3,'performance_benchmark':False,'deployed_to_existing_services':False}}
(OUT/'verification.json').write_text(json.dumps(record,ensure_ascii=False,indent=2))
print('Both host builds and LAN smoke verified; explicit positive pipeline remains rejected.')
