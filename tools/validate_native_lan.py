"""Windows two-host stage 6 acceptance, in new directories and on private test ports.

Requires an existing SSH key login, Python on both *test driver* machines, psutil
on the coordinator, local IQ3_S weights, and freshly built native node packages.
The deployed runtime directories contain no Python. Installs nothing, never edits
the input plan or installed services, and only stops processes started by this run.
"""
from __future__ import annotations

import argparse
import base64
import copy
from datetime import datetime, timezone
import hashlib
import io
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import sys
import tarfile
import time
import uuid

import psutil

from benchmark_native_runtime import request, save
from cluster_config import load_cluster

ROOT = Path(__file__).resolve().parents[1]


def quote_ps(value):
    return "'" + str(value).replace("'", "''") + "'"


def check_package(folder):
    meta = json.loads((folder / "CLUSTER_BUILD.json").read_text(encoding="utf-8"))
    files = {p.relative_to(folder).as_posix(): p for p in folder.rglob("*") if p.is_file()}
    if set(files) != set(meta["files"]) | {"CLUSTER_BUILD.json"}:
        raise AssertionError("package contains unlisted files")
    for name, digest in meta["files"].items():
        if hashlib.sha256(files[name].read_bytes()).hexdigest() != digest:
            raise AssertionError(f"package hash mismatch: {name}")
        if Path(name).suffix.lower() in (".py", ".pyc", ".pyo"):
            raise AssertionError("runtime package contains Python")
    return {"files_verified": len(meta["files"]), "artifacts": meta.get("artifacts", {}),
            "host_sha256": meta["files"]["strata-host.exe"],
            "backend_sha256": meta["files"]["strata_network_backend.dll"],
            "engine_sha256": meta["files"].get("strata.exe"),
            "cxx_release_flags": meta["cxx_release_flags"]}


def stop_local(process):
    if process.poll() is not None:
        return
    try:
        children = psutil.Process(process.pid).children(recursive=True)
    except psutil.NoSuchProcess:
        children = []
    process.terminate()
    process.wait(timeout=30)
    for child in children:
        try:
            child.terminate()
        except psutil.NoSuchProcess:
            pass
    _, alive = psutil.wait_procs(children, timeout=15)
    for child in alive:
        child.kill()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cluster-config", required=True, type=Path)
    ap.add_argument("--packages", required=True, type=Path)
    ap.add_argument("--ssh-user", required=True)
    ap.add_argument("--known-hosts", required=True, type=Path)
    ap.add_argument("--remote-python", required=True)
    ap.add_argument("--remote-parent", default="D:/Strata")
    ap.add_argument("--output", required=True, type=Path)
    ap.add_argument("--worker-port", type=int, default=18506)
    ap.add_argument("--api-port", type=int, default=18608)
    ap.add_argument("--context", type=int, default=8192)
    ap.add_argument("--prefill", type=int, default=512)
    ap.add_argument("--repeats", type=int, default=12)
    ap.add_argument("--expert-repeats", type=int, default=20)
    ap.add_argument("--max-tokens", type=int, default=128)
    ap.add_argument("--skip-golden", action="store_true")
    ap.add_argument("--resume", action="store_true", help="resume an interrupted run with the same plan and package hashes")
    ap.add_argument("--refresh-native", action="store_true",
                    help="with --resume, repeat C++ after a host rebuild; requires identical engine/backend hashes and flags")
    ap.add_argument("--rerun-native", action="store_true", help="with --resume, repeat the C++ run and preserve its prior reports")
    ap.add_argument("--trace-tokens", action="store_true", help="diagnostic run; capture C++ engine tokens (not a performance result)")
    args = ap.parse_args()
    if os.name != "nt":
        ap.error("this acceptance driver currently supports Windows only")
    original = load_cluster(args.cluster_config)
    if len(original.workers) != 1 or original.workers[0]["backend"] != "cpu":
        ap.error("this driver requires one remote CPU worker")
    folder = args.output.resolve()
    folder.mkdir(parents=True, exist_ok=args.resume)
    (folder / "logs").mkdir(exist_ok=True)
    previous = json.loads((folder / "acceptance.json").read_text(encoding="utf-8")) if args.resume else None
    remote = previous["remote_directory"] if previous else args.remote_parent.rstrip("/\\") + "/stage6-" + uuid.uuid4().hex[:12]
    if previous and not (remote.startswith(args.remote_parent.rstrip("/\\") + "/stage6-")
                         and len(remote.rsplit("stage6-", 1)[1]) == 12
                         and all(c in "0123456789abcdef" for c in remote.rsplit("stage6-", 1)[1])):
        raise ValueError("resume directory is outside this driver's acceptance directories")
    worker_id = original.workers[0]["id"]
    coordinator_id = original.coordinator["id"]
    worker_package = args.packages.resolve() / worker_id
    host_package = args.packages.resolve() / coordinator_id
    report = {"started_at_utc": datetime.now(timezone.utc).isoformat(), "remote_directory": remote,
              "packages": {worker_id: check_package(worker_package),
                           coordinator_id: check_package(host_package)},
              "source_plan_sha256": original.digest, "runs": {}, "complete": False,
              "scope": "Windows CUDA coordinator + LAN CPU worker; no HIP/SYCL execution"}
    if previous:
        if previous["source_plan_sha256"] != original.digest:
            raise ValueError("resume requires identical source plan and runtime packages")
        if previous["packages"] != report["packages"]:
            if not args.refresh_native:
                raise ValueError("runtime package changed; use a new output directory")
            for node_id, old in previous["packages"].items():
                new = report["packages"][node_id]
                for field in ("backend_sha256", "engine_sha256", "cxx_release_flags"):
                    if old[field] != new[field]:
                        raise ValueError("refresh requires the same engine, backend and CPU flags")
            previous.setdefault("native_refresh_history", []).append({
                "packages": previous["packages"], "run": previous["runs"].get("native"),
                "at_utc": datetime.now(timezone.utc).isoformat()})
            previous["packages"] = report["packages"]
            previous["runs"].pop("native", None)
            previous["complete"] = False
            archive_directory = folder / ("native-refresh-" + uuid.uuid4().hex[:8])
            archive_directory.mkdir()
            for path in folder.glob("native-*"):
                if path.is_file():
                    path.rename(archive_directory / path.name)
            golden = folder / "real-api-parity.json"
            if golden.exists():
                golden.rename(archive_directory / golden.name)
        if args.rerun_native:
            history = folder / ("native-repeat-" + datetime.now(timezone.utc).strftime("%H%M%S"))
            history.mkdir()
            for path in folder.glob("native-*"):
                if path.is_file():
                    path.rename(history / path.name)
            previous["runs"].pop("native", None)
            previous["complete"] = False
        report = previous
    ssh = ["ssh", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes", "-o",
           "UserKnownHostsFile=" + str(args.known_hosts.resolve()), "-o", "ConnectTimeout=10",
           "-l", args.ssh_user, original.workers[0]["host"]]
    remote_sessions = {}

    def remote_ps(script):
        # No command text is written to logs: the short-lived launch command sets
        # an ephemeral network secret in the child's environment.
        encoded = base64.b64encode(("$ErrorActionPreference='Stop'; $ProgressPreference='SilentlyContinue'; "
                                   + script + "; exit 0").encode("utf-16-le")).decode()
        result = subprocess.run(ssh + ["powershell -NoProfile -NonInteractive -EncodedCommand " + encoded],
                                capture_output=True, timeout=60)
        if result.returncode:
            raise RuntimeError("remote operation failed: " + result.stderr.decode(errors="replace")[-2000:])
        return result.stdout.decode("utf-8-sig", errors="replace").strip()

    def remote_start(exe, argv, name, native=False, env=None):
        secret = (env or {}).get("STRATA_NETWORK_KEY")
        assignments = "".join("$env:" + k + "=" + quote_ps(v) + ";" for k, v in (env or {}).items()
                              if k != "STRATA_NETWORK_KEY")
        if secret is not None:
            assignments += "$env:STRATA_NETWORK_KEY=[Console]::In.ReadLine();"
        if native:
            assignments += "$env:PATH=" + quote_ps(remote + "/runtime;C:/Windows/System32") + ";"
        argv_text = quote_ps(subprocess.list2cmdline(list(map(str, argv))))
        remote_ps("Remove-Item -LiteralPath " + quote_ps(remote + '/' + name + '.pid.json')
                  + " -ErrorAction SilentlyContinue")
        script = ("$ErrorActionPreference='Stop'; " + assignments +
            f"$p=Start-Process -WindowStyle Hidden -FilePath {quote_ps(exe)} -ArgumentList @({argv_text}) "
            f"-WorkingDirectory {quote_ps(remote)} -RedirectStandardOutput {quote_ps(remote + '/' + name + '.out.log')} "
            f"-RedirectStandardError {quote_ps(remote + '/' + name + '.err.log')} -PassThru; "
            "$p.Refresh(); @{pid=$p.Id;started=$p.StartTime.ToUniversalTime().Ticks.ToString()}|ConvertTo-Json -Compress|"
            f"Set-Content -Encoding UTF8 {quote_ps(remote + '/' + name + '.pid.json')}; $p.WaitForExit(); exit $p.ExitCode")
        encoded = base64.b64encode(script.encode("utf-16-le")).decode()
        log = (folder / (name + "-ssh.log")).open("wb")
        session = subprocess.Popen(ssh + ["powershell -NoProfile -NonInteractive -EncodedCommand " + encoded],
                                   stdin=subprocess.PIPE, stdout=log, stderr=log)
        if secret is not None:
            session.stdin.write((secret + "\n").encode("utf-8"))
            session.stdin.flush()
        session.stdin.close()
        # Windows OpenSSH contains its child processes in a job. Keep the SSH
        # command alive while its child runs; disconnecting a launcher kills it.
        for _ in range(40):
            data = remote_ps("if(Test-Path -LiteralPath " + quote_ps(remote + '/' + name + '.pid.json')
                             + "){Get-Content -Raw " + quote_ps(remote + '/' + name + '.pid.json') + "}")
            if data:
                identity = json.loads(data)
                remote_sessions[identity["pid"]] = (session, log)
                return identity
            if session.poll() is not None:
                log.close()
                raise RuntimeError("remote launch failed; inspect " + name + "-ssh.log")
            time.sleep(.25)
        session.terminate()
        session.wait(timeout=15)
        log.close()
        raise TimeoutError("remote launch did not publish its identity")

    def remote_stop(identity):
        if identity:
            remote_ps(f"$p=Get-Process -Id {identity['pid']} -ErrorAction SilentlyContinue; "
                      f"if($p -and $p.StartTime.ToUniversalTime().Ticks.ToString() -eq '{identity['started']}')"
                      "{Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; "
                      "try{$p.WaitForExit(30000)|Out-Null}catch{}}")
            session, log = remote_sessions.pop(identity["pid"])
            try:
                session.wait(timeout=30)
            finally:
                if session.poll() is None:
                    session.terminate()
                    session.wait(timeout=15)
                log.close()

    key = secrets.token_urlsafe(32)
    env = dict(os.environ, STRATA_NETWORK_KEY=key, STRATA_NETWORK_PROFILE="0")
    env.pop("STRATA_API_KEY", None)
    env["PYTHONPATH"] = os.pathsep.join([str(ROOT / "tmp/native-text-test-deps"), str(ROOT), str(ROOT / "tools")])
    plan = copy.deepcopy(original.data)
    worker = next(n for n in plan["nodes"] if n["id"] == worker_id)
    coord = next(n for n in plan["nodes"] if n["id"] == coordinator_id)
    worker.update(port=args.worker_port, library=remote + "/runtime/strata_network_backend.dll")
    sock = socket.socket()
    try:
        sock.connect((worker["host"], 22))
        coord["host"] = sock.getsockname()[0]
    finally:
        sock.close()
    source = json.loads(Path(coord["server_config"]).read_text(encoding="utf-8-sig"))
    source.update(port=args.api_port, host="127.0.0.1", open_browser=False,
                  lib_dirs=[str(host_package)], cwd=str(ROOT))
    source.pop("api_key", None)
    source.get("env", {}).pop("STRATA_API_KEY", None)
    if args.trace_tokens:
        source.setdefault("env", {})["STRATA_NATIVE_TOKEN_TRACE"] = str(folder / "raw-engine-tokens.log")
    coord.update(server_config=str(folder / "server.json"), engine=str(host_package / "strata.exe"),
                 context=args.context, prefill=args.prefill)
    save(folder / "server.json", source)
    save(folder / "cluster.json", plan)
    plan = load_cluster(folder / "cluster.json")
    local_pack = source["args"][source["args"].index("--pack") + 1]
    local_native = source["args"][source["args"].index("--native") + 1]
    report.update(config_sha256=plan.digest, coordinator_ipv4=coord["host"],
                  settings={"context": args.context, "prefill": args.prefill,
                            "worker_threads": worker["workers"], "ram_cache_mib": worker["ram_cache_mib"],
                            "preload_experts": worker.get("preload_experts", False),
                            "network_reduce": coord.get("network_reduce", False),
                            "max_tokens": args.max_tokens, "sampling_temperature": 0,
                            "network_profile": 0, "task_logging": False, "resource_sample_interval_seconds": .5})
    report["diagnostic_token_trace"] = args.trace_tokens
    if previous:
        if previous["config_sha256"] != plan.digest:
            raise ValueError("resume configuration changed")
    else:
        remote_ps(f"if(Test-Path -LiteralPath {quote_ps(remote)}){{throw 'acceptance directory exists'}}; "
                  f"New-Item -ItemType Directory -Path {quote_ps(remote)} | Out-Null")
    archive = io.BytesIO()
    with tarfile.open(fileobj=archive, mode="w") as tar:
        tar.add(worker_package, arcname="runtime")
        tar.add(folder / "cluster.json", arcname="cluster.json")
        for filename in ("network_worker.py", "gguf_reader.py", "cluster_config.py", "benchmark_native_runtime.py"):
            tar.add(ROOT / "tools" / filename, arcname="oracle/" + filename)
        tar.add(Path(psutil.__file__).parent, arcname="oracle/psutil")
    subprocess.run(ssh + [subprocess.list2cmdline(["tar.exe", "-xf", "-", "-C", remote])],
                   input=archive.getvalue(), check=True, timeout=120)
    remote_meta = json.loads(remote_ps(
        "@{os=(Get-CimInstance Win32_OperatingSystem).Caption; "
        "cpu=(Get-CimInstance Win32_Processor).Name; "
        "ram_kib=(Get-CimInstance Win32_OperatingSystem).TotalVisibleMemorySize}|ConvertTo-Json -Compress"))
    report["worker_hardware"] = remote_meta
    # Export once, so Python and C++ use exactly the same engine argv/env.
    if not previous:
        subprocess.run([str(host_package / "strata-host.exe"), "cluster", "--config", str(folder / "cluster.json"),
                        "--node", coordinator_id, "--export-config", str(folder / "runtime.json")],
                       env=env, cwd=folder, check=True, stdout=subprocess.DEVNULL)
    runtime = json.loads((folder / "runtime.json").read_text(encoding="utf-8"))
    if args.trace_tokens:
        runtime.setdefault("env", {})["STRATA_NATIVE_TOKEN_TRACE"] = str(folder / "raw-engine-tokens.log")
    else:
        runtime.get("env", {}).pop("STRATA_NATIVE_TOKEN_TRACE", None)
    save(folder / "runtime.json", runtime)
    report["engine_args"] = runtime["args"]
    report["engine_environment_settings"] = {k: v for k, v in runtime.get("env", {}).items()
                                             if k in ("STRATA_POOL_SPIN_US", "STRATA_FILE_RELEASE", "STRATA_UNBUFFERED_LOAD")}
    save(folder / "acceptance.json", report)
    for label in ("python", "native"):
        if report["runs"].get(label, {}).get("passed"):
            continue
        remote_worker, remote_sampler = None, None
        server, sampler = None, None
        try:
            if label == "native":
                worker_exe = remote + "/runtime/strata-host.exe"
                worker_argv = ["cluster", "--config", remote + "/cluster.json", "--node", worker_id]
            else:
                worker_exe = args.remote_python
                # The old CLI logs every task by default. Disable only that
                # constructor option in the test oracle, matching the native
                # worker's quiet default without editing the Python entry point.
                oracle = ("import sys;sys.path.insert(0," + repr(remote + "/oracle") + ");"
                          "import network_worker as w;original=w.Worker.__init__;"
                          "w.Worker.__init__=lambda self,*a,**k:original(self,*a,**dict(k,verbose=False));w.main()")
                worker_argv = ["-u", "-c", oracle, "--cluster-config", remote + "/cluster.json", "--node", worker_id]
            remote_worker = remote_start(worker_exe, worker_argv, label + "-worker", label == "native",
                                         {"STRATA_NETWORK_KEY": key, "STRATA_NETWORK_PROFILE": "0"})
            remote_sampler = remote_start(args.remote_python,
                [remote + "/oracle/benchmark_native_runtime.py", "sample", "--pid", str(remote_worker["pid"]),
                 "--label", label, "--report", remote + "/" + label + "-resources.json"], label + "-sampler")
            deadline = time.monotonic() + 1200
            while time.monotonic() < deadline:
                state = remote_ps(f"$p=Get-Process -Id {remote_worker['pid']} -ErrorAction SilentlyContinue; "
                    f"$log=(Get-Content {quote_ps(remote + '/' + label + '-worker.out.log')},{quote_ps(remote + '/' + label + '-worker.err.log')} "
                    "-ErrorAction SilentlyContinue | Select-Object -Last 4) -join [char]10; "
                    "@{alive=[bool]$p;log=$log}|ConvertTo-Json -Compress")
                state = json.loads(state)
                if not state["alive"]:
                    raise RuntimeError("remote worker exited: " + state["log"])
                if "worker ready:" in state["log"]:
                    break
                print(label + ": preload " + state["log"].splitlines()[-1] if state["log"] else label + ": starting", flush=True)
                time.sleep(5)
            else:
                raise TimeoutError("remote preload timed out")
            expert_report = folder / (label + "-experts.json")
            expert_done = json.loads(expert_report.read_text(encoding="utf-8")) if previous and expert_report.exists() else {}
            if not (expert_done.get("measurement_finished_at_utc") and expert_done.get("config_sha256") == plan.digest
                    and expert_done.get("repeats") == args.expert_repeats):
                subprocess.run([sys.executable, str(ROOT / "tools/benchmark_native_runtime.py"), "experts",
                                "--cluster-config", str(folder / "cluster.json"), "--node", worker_id,
                                "--pack", local_pack, "--native", local_native, "--label", label,
                                "--repeats", str(args.expert_repeats), "--report", str(expert_report)],
                               env=env, check=True, timeout=1200)
            if label == "native" and not args.skip_golden:
                subprocess.run([sys.executable, str(ROOT / "tools/test_native_server_real.py"), "--exe",
                                str(host_package / "strata-host.exe"), "--config", str(folder / "runtime.json"),
                                "--report", str(folder / "real-api-parity.json")], env=env, cwd=ROOT, check=True, timeout=2400)
            native_env = dict(env)
            native_env["PATH"] = os.pathsep.join([str(host_package), "C:/Windows/System32"])
            command = ([str(host_package / "strata-host.exe"), "cluster", "--config", str(folder / "cluster.json"),
                        "--node", coordinator_id] if label == "native" else
                       [sys.executable, "-m", "serve.server", "--engine", "strata", "--config", str(folder / "runtime.json"),
                        "--port", str(args.api_port)])
            with (folder / (label + "-server.log")).open("wb") as log:
                server = subprocess.Popen(command, cwd=folder if label == "native" else ROOT,
                                          env=native_env if label == "native" else env, stdout=log, stderr=log)
                sampler = subprocess.Popen([sys.executable, str(ROOT / "tools/benchmark_native_runtime.py"), "sample",
                    "--pid", str(server.pid), "--label", label, "--report", str(folder / (label + "-host-resources.json"))], env=env)
                deadline = time.monotonic() + 900
                while time.monotonic() < deadline:
                    if server.poll() is not None:
                        raise RuntimeError("coordinator exited; inspect its server log")
                    try:
                        if request(args.api_port, "/health").get("loaded"):
                            break
                    except (OSError, AssertionError):
                        pass
                    time.sleep(.5)
                else:
                    raise TimeoutError("coordinator READY timed out")
                engine_log = Path(runtime["log"])
                subprocess.run([sys.executable, str(ROOT / "tools/benchmark_native_runtime.py"), "api", "--port",
                                str(args.api_port), "--label", label, "--engine-log", str(engine_log),
                                "--repeats", str(args.repeats), "--max-tokens", str(args.max_tokens),
                                "--report", str(folder / (label + "-api.json"))], env=env, check=True, timeout=2400)
                if label == "native":
                    names = json.loads((folder / (label + "-host-resources.json")).read_text(encoding="utf-8"))["process_names"]
                    if any("python" in n.lower() for n in names):
                        raise AssertionError("native coordinator tree contains Python")
                (folder / (label + "-engine.log")).write_bytes(engine_log.read_bytes())
            report["runs"][label] = {"passed": True, "worker_pid": remote_worker["pid"], "coordinator_pid": server.pid}
        finally:
            if server:
                stop_local(server)
            if sampler:
                try:
                    sampler.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    stop_local(sampler)
            try:
                remote_stop(remote_worker)
            finally:
                remote_stop(remote_sampler)
                # Retrieve only nonsecret logs and sampling reports. Runtime directory remains available for review.
                for name in (label + "-resources.json", label + "-worker.out.log", label + "-worker.err.log"):
                    data = remote_ps(f"if(Test-Path -LiteralPath {quote_ps(remote + '/' + name)})"
                                     f"{{[Convert]::ToBase64String([IO.File]::ReadAllBytes({quote_ps(remote + '/' + name)}))}}")
                    if data:
                        (folder / name).write_bytes(base64.b64decode(data))
            save(folder / "acceptance.json", report)
        time.sleep(2)
    python_experts = json.loads((folder / "python-experts.json").read_text(encoding="utf-8"))["groups"]
    native_experts = json.loads((folder / "native-experts.json").read_text(encoding="utf-8"))["groups"]
    report["expert_outputs_equal"] = len(python_experts) == len(native_experts) and all(
        p["output_sha256"] == n["output_sha256"] for p, n in zip(python_experts, native_experts))
    def outputs(label):
        return [(s["prompt"], s["output_sha256"], s["usage"], s["finish"])
                for s in json.loads((folder / (label + "-api.json")).read_text(encoding="utf-8"))["samples"]]
    report["api_outputs_and_usage_equal"] = outputs("python") == outputs("native")
    names = json.loads((folder / "native-resources.json").read_text(encoding="utf-8"))["process_names"]
    report["native_worker_tree_has_python"] = any("python" in n.lower() for n in names)
    report["complete"] = report["expert_outputs_equal"] and report["api_outputs_and_usage_equal"] and not report["native_worker_tree_has_python"]
    save(folder / "acceptance.json", report)
    if not report["complete"]:
        raise AssertionError("LAN acceptance mismatch; inspect acceptance.json")


if __name__ == "__main__":
    main()
