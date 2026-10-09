"""One bounded full-engine generation using the deployed LAN worker; no API changes."""
import json
import argparse
import os
from pathlib import Path
import queue
import re
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from batch_test import tokenizer
from cluster_config import load_cluster
from make_network_config import make_config


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=('raw', 'reduce', 'local', 'configured'), default='raw')
    parser.add_argument('--engine', default=None)
    parser.add_argument('--prefix', default='network-opt-full-decode')
    parser.add_argument('--local-ram-mib', type=int, default=4096)
    parser.add_argument('--vram-reserve-mib', type=int, default=0)
    parser.add_argument('--expert-cache', type=int, default=0)
    parser.add_argument('--pool-affinity', choices=('all', 'auto', 'p-cores'))
    parser.add_argument('--pool-spin-us', type=int)
    cli = parser.parse_args()
    plan = load_cluster(ROOT / 'tmp/dual-host-test/cluster.json')
    node = plan.coordinator
    source = json.loads(Path(node['server_config']).read_text())
    use_reduce = node.get('network_reduce', False) if cli.mode == 'configured' else cli.mode != 'raw'
    local_budget = node.get('network_local_ram_mib', 0) if cli.mode == 'configured' else (cli.local_ram_mib if cli.mode == 'local' else 0)
    cfg = make_config(source, plan.endpoints, node['context'], cli.engine or node['engine'],
                      use_reduce, local_budget)
    args = cfg['args'] + ['--prefill', str(node['prefill']), '--network-assignment', plan.assignment,
                          '--network-config-id', plan.digest]
    if cli.vram_reserve_mib:
        args += ['--vram-reserve-mib', str(cli.vram_reserve_mib)]
    if cli.expert_cache:
        args += ['--expert-cache', str(cli.expert_cache)]
    if cli.pool_affinity:
        args += ['--pool-affinity', cli.pool_affinity]
    actual_affinity = 'all'
    if '--pool-affinity' in args:
        affinity_at = len(args) - 1 - args[::-1].index('--pool-affinity')
        actual_affinity = args[affinity_at + 1]
    env = dict(os.environ)
    env.update({str(k): str(v) for k, v in cfg.get('env', {}).items()})
    if cli.pool_spin_us is not None:
        env['STRATA_POOL_SPIN_US'] = str(cli.pool_spin_us)
    env['STRATA_NETWORK_KEY'] = (ROOT / 'tmp/dual-host-test/network.key').read_text().strip()
    env['PATH'] = os.pathsep.join([str(Path(cfg['exe']).parent), *cfg.get('lib_dirs', []), env.get('PATH', '')])
    tok = tokenizer(cfg['tokenizer'])
    prompt = ('<|im_start|>user\nWrite a Python function to merge overlapping intervals, '
              'including type hints, input validation, and a short explanation with an example.'
              '<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n')
    ids = tok.encode(prompt, parse_special=True)
    log_path = ROOT / f'tmp/{cli.prefix}.log'
    messages = queue.Queue()
    start = time.perf_counter()
    with log_path.open('w') as log:
        process = subprocess.Popen([cfg['exe'], '--serve', *args], cwd=ROOT, env=env,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, text=True, bufsize=1,
            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        def reader():
            for line in process.stdout:
                messages.put(line.strip())
            messages.put(None)
        thread = threading.Thread(target=reader, daemon=True)
        thread.start()
        def next_line(deadline):
            try:
                line = messages.get(timeout=max(.01, deadline - time.monotonic()))
            except queue.Empty:
                raise TimeoutError(f'engine timeout; see {log_path}') from None
            if line is None or line.startswith('ERR'):
                raise RuntimeError(f'engine ended or failed: {line}; see {log_path}')
            return line
        try:
            deadline = time.monotonic() + 180
            while not next_line(deadline).startswith('READY'):
                pass
            startup = time.perf_counter() - start
            print(f'Engine READY after {startup:.1f}s; prompt {len(ids)} tokens; max output 256.', flush=True)
            gen_start = time.perf_counter()
            process.stdin.write('GEN 256 temperature=0 ' + ','.join(map(str, ids)) + '\n')
            process.stdin.flush()
            tokens = []
            deadline = time.monotonic() + 180
            while True:
                line = next_line(deadline)
                if line.startswith('T '):
                    tokens.append(int(line.split()[1]))
                    if len(tokens) % 32 == 0:
                        print(f'Generated {len(tokens)} tokens.', flush=True)
                if line.startswith('DONE'):
                    break
            elapsed = time.perf_counter() - gen_start
            process.stdin.write('QUIT\n')
            process.stdin.flush()
            process.wait(timeout=15)
            if process.returncode or not tokens:
                raise RuntimeError(f'engine exit {process.returncode}, output tokens={len(tokens)}')
            result = dict(mode=cli.mode, local_ram_mib=local_budget,
                          pool_affinity=actual_affinity,
                          pool_spin_us=int(env.get('STRATA_POOL_SPIN_US', '20000')),
                          context=node['context'], spec=4, prompt_tokens=len(ids), output_tokens=len(tokens),
                          startup_seconds=round(startup, 3), request_seconds=round(elapsed, 3),
                          text=tok.decode(tokens), tokens=tokens, notes='Single short-prompt run; compare only runs with matching model, prompt and settings.')
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
            thread.join(2)
    text = log_path.read_text()
    match = re.search(r'(\d+) generated in (\d+) ms \(([\d.]+) tok/s\)', text)
    if match:
        result.update(engine_output_tokens=int(match[1]), decode_ms=int(match[2]), decode_tps=float(match[3]))
    match = re.search(r'expert cache (\d+) slots, ([\d.]+) GiB', text)
    if match:
        result.update(cache_slots=int(match[1]), cache_gib=float(match[2]))
    matches = re.findall(r'strata network local: entries=(\d+)', text)
    if matches:
        result['local_entries'] = int(matches[-1])
    match = re.search(r'resident RAM mode: ([\d.]+) GiB', text)
    if match:
        result['resident_ram_gib'] = float(match[1])
    match = re.search(r'strata network: .*?requests=(\d+) entries=(\d+) tx=(\d+) rx=(\d+) roundtrip_ms=([\d.]+)', text)
    if match:
        result['network'] = dict(zip(('requests', 'entries', 'tx', 'rx'), map(int, match.groups()[:4])))
        result['network']['roundtrip_ms'] = float(match[5])
    (ROOT / f'tmp/{cli.prefix}.json').write_text(json.dumps(result, indent=2) + '\n')
    if local_budget and not result.get('local_entries'):
        raise RuntimeError('Local mode executed no CPU experts; inspect RAM/commit headroom in the log.')
    print(json.dumps({k: v for k, v in result.items() if k not in ('text', 'tokens')}, indent=2))


if __name__ == '__main__':
    main()
