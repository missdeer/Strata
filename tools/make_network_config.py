"""Create a separate coordinator config from an installed model config; never change the source."""
import argparse
import ipaddress
import json
from pathlib import Path


def endpoints(value):
    peers = value.split(",")
    if not 1 <= len(peers) <= 16 or len(set(peers)) != len(peers):
        raise ValueError("choose 1..16 distinct worker addresses")
    for peer in peers:
        host, port = peer.rsplit(":", 1)
        ipaddress.IPv4Address(host)
        if not port.isdigit() or not 1 <= int(port) <= 65535:
            raise ValueError("worker port must be 1..65535")
    return value


def make_config(source, peers, context=131072, exe=None, reduce=False, local_ram_mib=0):
    cfg = dict(source)
    if "--native" not in cfg.get("args", []):
        raise ValueError("network workers require an installed native IQ model")
    remove_values = {"--network-workers", "--network-timeout-ms", "--network-assignment", "--network-config-id",
        "--max-context", "--prefill", "--batch", "--slots",
        "--layer-split", "--split-device", "--peer-device", "--expert-cache-device1", "--expert-cache-device2",
        "--expert-cache-device3", "--pipeline-windows", "--shared-expert-arena", "--resident-budget-gib", "--kv-resident",
        "--pcie-frac", "--short-read", "--gpu", "--network-local-ram-mib"}
    remove_flags = {"--no-pool", "--resident-experts", "--resident-cpu-experts", "--resident-cpu-experts-soft",
        "--remote-expert-opt", "--split-skip-if-fits", "--trim-stage-weights", "--mmap-experts", "--network-reduce"}
    args, old, i = [], cfg["args"], 0
    while i < len(old):
        if old[i] in remove_values:
            if i + 1 == len(old):
                raise ValueError(f"missing value for {old[i]}")
            i += 2
        elif old[i] in remove_flags:
            i += 1
        else:
            args.append(old[i]); i += 1
    args += ["--network-workers", endpoints(peers), "--network-timeout-ms", "5000", "--mmap-experts",
             "--pcie-frac", "0", "--short-read", "0", "--max-context", str(context), "--prefill", "512"]
    if reduce:
        args.append("--network-reduce")
    if local_ram_mib:
        args += ["--network-local-ram-mib", str(local_ram_mib)]
    cfg["args"] = args
    cfg["gpu"] = 0
    cfg["host"] = "127.0.0.1"
    cfg["model_name"] = cfg.get("model_name", "strata") + "-network"
    cfg["open_browser"] = False
    for name in ("layer_split", "split_skip_if_fits", "batch", "batch_groups", "pipeline_windows"):
        cfg.pop(name, None)
    if exe:
        cfg["exe"] = str(Path(exe).resolve())
    return cfg


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--workers", default="172.16.0.11:18500,172.16.0.17:18500")
    p.add_argument("--context", type=int, default=131072)
    p.add_argument("--exe", help="new coordinator engine, if different from the installed one")
    p.add_argument("--force", action="store_true", help="overwrite the destination, never the source")
    a = p.parse_args()
    src, dst = Path(a.source), Path(a.out)
    if src.resolve() == dst.resolve():
        p.error("source and destination must differ")
    if dst.exists() and not a.force:
        p.error("destination exists; use a new path or --force")
    if not 1024 <= a.context <= 131072:
        p.error("context must be 1024..131072 tokens")
    cfg = make_config(json.loads(src.read_text(encoding="utf-8")), a.workers, a.context, a.exe)
    cfg["log"] = str(dst.resolve().with_suffix(".log"))
    with dst.open("w" if a.force else "x", encoding="utf-8") as f:
        json.dump(cfg, f, ensure_ascii=False, indent=2)
        f.write("\n")
    print(f"Created {dst}; set the same STRATA_NETWORK_KEY on coordinator and workers before starting.")


if __name__ == "__main__":
    main()
