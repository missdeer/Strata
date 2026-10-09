"""Same-machine before/after worker benchmark, with identical warm expert jobs."""
import argparse
import importlib.util
import json
import math
from pathlib import Path
import statistics
import struct
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('before', 'after'))
    args = parser.parse_args()
    module = ROOT / ('tmp/network-opt-baseline/network_worker.py' if args.mode == 'before' else 'tools/network_worker.py')
    library = ROOT / ('tmp/network-opt-baseline/strata_network_backend.dll' if args.mode == 'before'
                      else 'dist/cluster-network-opt/host11/strata_network_backend.dll')
    spec = importlib.util.spec_from_file_location('measured_worker', module)
    W = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(W)
    source = W.ExpertFiles('D:/Projects/Strata-data/packs/iq3_s',
        'D:/Projects/Strata-data/models/IQ3_S/Qwen3.8-Flash-Next-GSQ-RCO-IQ3_S-00001-of-00002.gguf', cache_mib=128)
    backend = W.NativeBackend(library, workers=7)
    result = {'mode': args.mode, 'host': 'host16 i9-12900HK, Windows',
              'notes': 'Small warm working set; not full-model decode or host11 timings.', 'groups': []}
    try:
        for layer in (0, 1, 2, 17, 47):
            for nt in (1, 3, 4):
                entries = [(t * 10 + j, t * 6 + j) for t in range(nt) for j in range(6)]
                activation = struct.pack('<f', .125) * (nt * W.H)
                for _, e in entries:
                    source.pointer(layer, e)
                def run():
                    return backend.compute(source, layer, nt, 10, entries, activation)
                for _ in range(5):
                    data = run()
                samples, validation = [], []
                for _ in range(40):
                    start = time.perf_counter()
                    data = run()
                    samples.append((time.perf_counter() - start) * 1000)
                    if hasattr(backend, 'last_timings'):
                        validation.append(backend.last_timings['validate_ms'])
                import hashlib
                row = dict(layer=layer, positions=nt, entries=len(entries),
                           median_ms=round(statistics.median(samples), 4),
                           mean_ms=round(statistics.mean(samples), 4), sha256=hashlib.sha256(data).hexdigest())
                if validation:
                    row['validate_median_ms'] = round(statistics.median(validation), 4)
                result['groups'].append(row)
        output = ROOT / f'tmp/network-opt-local-{args.mode}.json'
        output.write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps(result, indent=2))
    finally:
        backend.close()
        source.close()


if __name__ == '__main__':
    main()
