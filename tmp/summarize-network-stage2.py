import json
from pathlib import Path
import re
import statistics

root = Path(__file__).resolve().parent
names = ['network-stage2-fixed-raw', 'network-stage2-fixed-reduce', 'network-stage2-reduce-r2',
         'network-stage2-local-1g', 'network-stage2-local-1g-r2']
rows = []
for name in names:
    data = json.loads((root / (name + '.json')).read_text())
    row = {k: v for k, v in data.items() if k not in ('text', 'tokens', 'notes')}
    row['artifact'] = name
    match = re.search(r'drafts accepted (\d+) of (\d+)', (root / (name + '.log')).read_text())
    if match:
        row['drafts_accepted'], row['drafts_total'] = map(int, match.groups())
    rows.append(row)
results = {'matched_runs': rows,
           'raw_reduce_256_tokens_identical': json.loads((root / (names[0] + '.json')).read_text())['tokens'] == json.loads((root / (names[1] + '.json')).read_text())['tokens'],
           'local_repeats_256_tokens_identical': json.loads((root / (names[3] + '.json')).read_text())['tokens'] == json.loads((root / (names[4] + '.json')).read_text())['tokens'],
           'reduce_mean_decode_ms': statistics.mean(r['decode_ms'] for r in rows[1:3]),
           'local_mean_decode_ms': statistics.mean(r['decode_ms'] for r in rows[3:5]),
           'notes': 'Short prompt, max context 131072, fixed 3107 GPU cache slots, 256 output tokens per run. Not a long-conversation benchmark.'}
(root / 'network-stage2-summary.json').write_text(json.dumps(results, indent=2) + '\n')
print(json.dumps(results, indent=2))
