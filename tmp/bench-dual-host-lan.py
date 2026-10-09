"""Bounded expert-only benchmark against the existing worker; never restarts services."""
import argparse
import json
import math
from pathlib import Path
import random
import socket
import statistics
import struct
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import network_worker as W
from cluster_config import load_cluster
from gguf_reader import GGML_TYPES


def summary(samples):
    ordered = sorted(samples)
    return {'count': len(samples), 'median_ms': round(statistics.median(samples), 3),
            'mean_ms': round(statistics.mean(samples), 3),
            'p95_ms': round(ordered[math.ceil(.95 * len(ordered)) - 1], 3),
            'min_ms': round(ordered[0], 3), 'max_ms': round(ordered[-1], 3)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repeats', type=int, default=12)
    parser.add_argument('--output', type=Path, default=ROOT / 'tmp/dual-host-lan-benchmark.json')
    parser.add_argument('--reduce', action='store_true')
    args = parser.parse_args()
    if not 1 <= args.repeats <= 50:
        parser.error('repeats must be 1..50')
    plan = load_cluster(ROOT / 'tmp/dual-host-test/cluster.json')
    node = plan.workers[0]
    if len(plan.workers) != 1 or node['id'] != 'host11':
        raise ValueError('this benchmark requires the host11-only expert assignment')
    source = W.ExpertFiles('D:/Projects/Strata-data/packs/iq3_s',
        'D:/Projects/Strata-data/models/IQ3_S/Qwen3.8-Flash-Next-GSQ-RCO-IQ3_S-00001-of-00002.gguf', cache_mib=64)
    try:
        fingerprint = source.fingerprint()
        formats = [(GGML_TYPES[l[0]], GGML_TYPES[l[1]]) for l in source.layers]
    finally:
        source.close()
    key = (ROOT / 'tmp/dual-host-test/network.key').read_bytes().strip()
    hello = (struct.pack('<QIIIII', fingerprint, 48, 512, 0, 1, W.H)
             + plan.digest.encode() + plan.assignment.encode() + key)
    result = {'endpoint': plan.endpoints, 'config_sha256': plan.digest,
              'repeats': args.repeats, 'reduce': args.reduce, 'groups': [], 'layer_sweeps': [], 'tasks': [],
              'notes': ['Client wall timings include computation, validation, TCP and scheduling.',
                        'This does not measure complete model TPS.',
                        'Warmup calls are excluded from group timing summaries.']}
    rng = random.Random(16411)
    sequence = 0
    def request(conn, layer, nt, width, sharing, rotate=False):
        nonlocal sequence
        offset = rng.randrange(512) if rotate else 0
        entries = [(t * 10 + j, ((j if sharing else t * width + j) + offset) % 512)
                   for t in range(nt) for j in range(width)]
        activation = struct.pack('<f', .125) * (nt * W.H)
        body = (struct.pack('<IIII', layer, nt, 10, len(entries))
                + b''.join(struct.pack('<IIf', *entry, 1. / width) if args.reduce
                           else struct.pack('<II', *entry) for entry in entries) + activation)
        sequence += 1
        start = time.perf_counter()
        W.send(conn, sequence, 4 if args.reduce else 2, body, 10)
        seq, kind, reply = W.frame(conn, 10)
        elapsed = (time.perf_counter() - start) * 1000
        if (seq, kind) != (sequence, 132 if args.reduce else 130):
            raise RuntimeError(f'worker error: sequence={seq}, kind={kind}, message={reply[:256]!r}')
        if len(reply) != (nt if args.reduce else len(entries)) * W.H * 4:
            raise RuntimeError('wrong result size')
        result['tasks'].append(dict(sequence=sequence, layer=layer, positions=nt,
                                    entries=len(entries), elapsed_ms=round(elapsed, 4)))
        return elapsed
    start = time.perf_counter()
    with socket.create_connection((node['host'], node['port']), timeout=5) as conn:
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        W.send(conn, 0, 6 if args.reduce else 3, hello, 10)
        if W.frame(conn, 10) != (0, 134 if args.reduce else 131, b''):
            raise RuntimeError('cluster handshake rejected')
        print('Authenticated host11 benchmark; model/config identity matched.', flush=True)
        shapes = [(1, 1, True), (1, 6, True), (3, 1, True),
                  (3, 6, True), (3, 6, False), (4, 6, False)]
        for layer in (0, 1, 2, 17, 47):
            for nt, width, shared in shapes:
                for _ in range(2):
                    request(conn, layer, nt, width, shared)
                samples = [request(conn, layer, nt, width, shared) for _ in range(args.repeats)]
                row = dict(layer=layer, gate_up=formats[layer][0], down=formats[layer][1],
                           positions=nt, entries=nt * width,
                           distinct_experts=width if shared else nt * width, **summary(samples))
                result['groups'].append(row)
                print(json.dumps(row), flush=True)
        for shared in (True, False):
            samples = []
            layers = []
            for sweep in range(3):
                one = [request(conn, layer, 3, 6, shared, rotate=True) for layer in range(48)]
                samples.extend(one)
                layers.append(one)
            row = dict(positions=3, entries_per_layer=18,
                       distinct_experts_per_layer=6 if shared else 18,
                       sweep_total_ms=[round(sum(s), 3) for s in layers], **summary(samples))
            result['layer_sweeps'].append(row)
            print('48-layer sweep: ' + json.dumps(row), flush=True)
    result['elapsed_seconds'] = round(time.perf_counter() - start, 3)
    result['requests'] = sequence
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(f'Completed {sequence} requests in {result["elapsed_seconds"]} seconds.', flush=True)


if __name__ == '__main__':
    main()
