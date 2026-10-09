import json
from pathlib import Path
import time
import urllib.request

root = Path(__file__).resolve().parents[1]
key = (root / 'tmp/dual-host-test/network.key').read_text().strip()
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
headers = {'Authorization': 'Bearer ' + key, 'Content-Type': 'application/json'}
deadline = time.monotonic() + 180
health = None
while time.monotonic() < deadline:
    try:
        req = urllib.request.Request('http://127.0.0.1:8080/health', headers=headers)
        with opener.open(req, timeout=3) as response:
            health = json.load(response)
        if health.get('loaded'):
            break
    except (OSError, ValueError):
        pass
    time.sleep(2)
else:
    raise SystemExit('API did not become ready; inspect the coordinator startup logs.')
req = urllib.request.Request('http://127.0.0.1:8080/v1/models', headers=headers)
with opener.open(req, timeout=5) as response:
    models = json.load(response)
model = 'qwen3.8-flash-next-iq3_s-network'
if model not in [m['id'] for m in models['data']]:
    raise SystemExit('API model mismatch')
body = {'model': model, 'messages': [{'role': 'user', 'content': '请直接回答：2+2等于几？'}],
        'temperature': 0, 'reasoning_effort': 'none', 'max_tokens': 8}
req = urllib.request.Request('http://127.0.0.1:8080/v1/chat/completions', data=json.dumps(body).encode(), headers=headers)
with opener.open(req, timeout=120) as response:
    result = json.load(response)
answer = result['choices'][0]['message'].get('content', '')
if '4' not in answer:
    raise SystemExit('API generation check failed: ' + repr(answer))
saved = {'model': model, 'loaded': True, 'answer': answer, 'usage': result.get('usage')}
(root / 'tmp/network-stage2-api-check.json').write_text(json.dumps(saved, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
print(json.dumps(saved, ensure_ascii=False))
