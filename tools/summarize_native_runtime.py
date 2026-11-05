"""Summarize a completed Windows LAN acceptance run, retaining measurement limits."""
import argparse
from datetime import datetime
import json
from pathlib import Path
import statistics

from benchmark_native_runtime import save


def read(path):
    return json.loads(path.read_text(encoding="utf-8"))


def resources(data, intervals):
    samples = [s for s in data["samples"] if any(start <= s["at_utc"] <= end for start, end in intervals)]
    if not samples:
        raise ValueError("no resource samples overlap the measured requests")
    gpu = [s["nvidia"] for s in samples if s["nvidia"] is not None]
    return {"samples": len(samples), "tree_cpu_percent_mean": statistics.mean(s["tree_cpu_percent"] for s in samples),
            "tree_cpu_whole_machine_percent_mean": statistics.mean(s["tree_cpu_percent"] for s in samples) / data["logical_cpus"],
            "tree_rss_max_gib": max(s["tree_rss_bytes"] for s in samples) / 2**30,
            "system_ram_max_gib": max(s["system_ram_used_bytes"] for s in samples) / 2**30,
            "gpu_percent_mean": statistics.mean(s["gpu_percent"] for s in gpu) if gpu else None,
            "vram_max_gib": max(s["vram_used_bytes"] for s in gpu) / 2**30 if gpu else None,
            "process_names": data["process_names"],
            "entire_run_tree_rss_peak_gib": data["peak_tree_rss_bytes"] / 2**30}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    root = args.run.resolve()
    acceptance = read(root / "acceptance.json")
    if acceptance["settings"].get("task_logging") is not False or acceptance.get("diagnostic_token_trace"):
        raise ValueError("require matching, disabled task logging and no token trace for performance results")
    report = {"scope": acceptance["scope"], "config_sha256": acceptance["config_sha256"],
              "packages": acceptance["packages"], "worker_hardware": acceptance["worker_hardware"],
              "settings": acceptance["settings"], "engine_args": acceptance["engine_args"],
              "engine_environment_settings": acceptance["engine_environment_settings"],
              "raw_run_directory": str(root), "runs": {},
              "acceptance_complete": acceptance["complete"],
              "validation": {key: acceptance[key] for key in ("expert_outputs_equal", "api_outputs_and_usage_equal",
                                                              "native_worker_tree_has_python")},
              "limitations": ["Sequential Python/C++ comparison, one model and one Windows LAN pair.",
                              "Nearest-rank p95; with 12 API samples p95 is the maximum sample.",
                              "Warmups are excluded; initial load, prefill and cache reuse are reported separately.",
                              "CPU/RAM/VRAM samples use 0.5-second intervals and can miss peaks.",
                              "System RAM and GPU measurements include other programs.",
                              "No fresh Windows VM, Linux, remote HIP GPU, Intel Arc inference or vision model test.",
                              "The host11 /QxHost build was checked for correctness separately; this pair uses the portable backend."]}
    days = set()
    for label in ("python", "native"):
        api = read(root / (label + "-api.json"))
        expert = read(root / (label + "-experts.json"))
        days.update([datetime.fromisoformat(api["measured_at_utc"]).date(),
                     datetime.fromisoformat(expert["measured_at_utc"]).date()])
        measured = [s for s in api["samples"] if not s["warmup"]]
        intervals = [(s["started_at_utc"], s["finished_at_utc"]) for s in measured]
        communications = {prompt: {field: statistics.median(sum(s["network_counters_delta"].get(host, {}).get(field, 0)
                                                               for host in s["network_counters_delta"])
                                                          for s in measured if s["prompt"] == prompt)
                                   for field in ("tasks", "entries", "tx", "rx")}
                          for prompt in ("code", "long")}
        report["runs"][label] = {"expert_groups": expert["groups"], "api_groups": api["groups"],
                                "api_samples": api["samples"], "effective_engine": api["engine"],
                                "network_bytes_median_per_request": communications,
                                "coordinator_resources": resources(read(root / (label + "-host-resources.json")), intervals),
                                "worker_resources": resources(read(root / (label + "-resources.json")), intervals)}
    if len(days) != 1:
        raise ValueError("Python/C++ measurements were made on different UTC days")
    report["measured_day_utc"] = next(iter(days)).isoformat()
    save(args.output, report)
    for label, run in report["runs"].items():
        print(label, [(g["prompt"], round(g["wall_ms"]["p50"], 2), g["decode_tok_s"]["p50"])
                      for g in run["api_groups"]])


if __name__ == "__main__":
    main()
