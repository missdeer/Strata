"""Read saved LAN timings and measure worker overhead locally; no service changes."""
import ctypes as C
import json
import math
from pathlib import Path
import re
import statistics
import struct
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import network_worker as W
from gguf_reader import GGML_TYPES


def bench(fn, repeats=30):
    fn()
    samples = []
    for _ in range(repeats):
        start = time.perf_counter()
        fn()
        samples.append((time.perf_counter() - start) * 1000)
    return round(statistics.median(samples), 4)


def saved_requests():
    results = []
    previous = [0, 0, 0, 0, 0.0]
    current = None
    for line in (ROOT / 'logs/cluster-host16.log').read_text().splitlines():
        if 'engine started:' in line:
            previous = [0, 0, 0, 0, 0.0]
        match = re.search(r'(\d+) generated in (\d+) ms', line)
        if match:
            current = {'output_tokens': int(match[1]), 'decode_ms': int(match[2])}
        match = re.search(r'192\.168\.233\.126:18500 requests=(\d+) entries=(\d+) tx=(\d+) rx=(\d+) roundtrip_ms=([\d.]+)', line)
        if match and current:
            cumulative = [int(v) for v in match.groups()[:4]] + [float(match[5])]
            delta = [a - b for a, b in zip(cumulative, previous)]
            previous = cumulative
            req, entries, tx, rx, ms = delta
            current.update(tasks=req, entries=entries, tx_bytes=tx, rx_bytes=rx,
                           roundtrip_ms=round(ms, 3), mean_task_ms=round(ms / req, 3),
                           roundtrip_decode_ratio=round(ms / current['decode_ms'], 4),
                           min_full_duplex_1g_transfer_ms=round(max(tx, rx) / 125000, 3))
            results.append(current)
    return results


def main():
    result = {'saved_lan_requests': saved_requests(), 'measurement_host': 'host16; not host11',
              'limitations': [
                  'Native timings use a small warm working set, not a full-model decode.',
                  'Roundtrip intervals include overlapped GPU work and host scheduling; they are not pure network or stall times.',
                  '1 Gbit/s transfer values are ideal lower bounds, not measured link times.',
              ]}
    checks = []
    for nt, n in [(1, 6), (3, 19), (4, 24)]:
        activation = struct.pack('<f', .125) * (nt * W.H)
        output = struct.pack('<f', .25) * (n * W.H)
        check = lambda data: all(math.isfinite(v[0]) for v in struct.iter_unpack('<f', data))
        checks.append({'positions': nt, 'entries': n,
                       'input_finite_ms': bench(lambda: check(activation)),
                       'output_finite_ms': bench(lambda: check(output))})
    result['python_validation'] = checks
    source = W.ExpertFiles('D:/Projects/Strata-data/packs/iq3_s',
        'D:/Projects/Strata-data/models/IQ3_S/Qwen3.8-Flash-Next-GSQ-RCO-IQ3_S-00001-of-00002.gguf',
        cache_mib=256)
    formats = {}
    for l, (gu, down, *_) in enumerate(source.layers):
        key = (GGML_TYPES[gu], GGML_TYPES[down])
        formats.setdefault(key, []).append(l)
    result['pack_formats'] = [{'gate_up': k[0], 'down': k[1], 'layers': v} for k, v in formats.items()]
    backend = W.NativeBackend(ROOT / 'dist/cluster/host11/strata_network_backend.dll', workers=7)
    try:
        measurements = []
        for layers in formats.values():
            l = layers[0]
            for nt in (1, 3):
                # Six experts per position; three shared, three distinct between positions.
                entries = [(t * 10 + j, j if j < 3 else 3 + t * 3 + j - 3)
                           for t in range(nt) for j in range(6)]
                for _, e in entries:
                    source.pointer(l, e)
                n = len(entries)
                rows = (C.c_int32 * n)(*(r for r, _ in entries))
                ids = (C.c_int32 * n)(*(e for _, e in entries))
                buffers = [source.pointer(l, e) for _, e in entries]
                pointers = (C.c_void_p * n)(*(C.addressof(b) for b in buffers))
                x = (C.c_float * (nt * W.H))(*([.125] * (nt * W.H)))
                out = (C.c_float * (n * W.H))()
                gu, down = source.layers[l][:2]
                def native():
                    if not backend.lib.strata_network_compute(backend.handle, gu, down, l, x, nt, n,
                                                             rows, ids, pointers, 10, out):
                        raise RuntimeError(backend.error())
                activation = bytes(x)
                measurements.append({'layer': l, 'positions': nt, 'entries': n,
                    'native_call_ms': bench(native, 12),
                    'python_plus_native_ms': bench(lambda: backend.compute(source, l, nt, 10, entries, activation), 12)})
        result['local_warm_backend'] = measurements
    finally:
        backend.close()
        source.close()
    text = json.dumps(result, indent=2)
    (ROOT / 'tmp/dual-host-costs.json').write_text(text + '\n')
    print(text)


if __name__ == '__main__':
    main()
