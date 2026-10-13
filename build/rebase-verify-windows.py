"""Verify freshly built Windows products without running model inference."""
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
MATRIX = {
    "cpu": ("build-network", "Release", None),
    "cuda": ("build-intel", "Release", "sm_89"),
    "hip": ("build-cluster/host17-core", "Release", "gfx1102;gfx1103"),
    "sycl": ("build-sycl-text", "Debug", "SPIR-V JIT"),
}
REPORT = {
    "checked_at_utc": datetime.now(timezone.utc).isoformat(),
    "source_commit": subprocess.check_output(
        ["git", "-c", f"safe.directory={ROOT.as_posix()}", "rev-parse", "HEAD"],
        cwd=ROOT, text=True).strip(),
    "platform": "Windows x64",
    "gpu_inference_tested": False,
    "uncommitted_build_fixes": {
        path: hashlib.sha256((ROOT / path).read_bytes()).hexdigest()
        for path in ("sycl/src/core/pinned.dp.cpp", "sycl/src/kernels/cuda/verify_kernels.dp.cpp")
    },
    "backends": {},
}

def check(name, args, env):
    result = subprocess.run(args, cwd=ROOT, env=env, capture_output=True,
                            encoding="utf-8", errors="replace", timeout=120)
    log = ROOT / "tmp" / f"rebase-verify-{name}.log"
    log.write_text(result.stdout + result.stderr, encoding="utf-8")
    print(f"{name}: {'PASS' if result.returncode == 0 else 'FAIL'}", flush=True)
    return {"exit_code": result.returncode, "log": str(log.relative_to(ROOT))}

failed = False
network_only = "--network-only" in sys.argv
report_path = ROOT / "tmp/rebase-windows-verification.json"
if report_path.exists():
    previous = json.loads(report_path.read_text(encoding="utf-8"))
    if previous["source_commit"] == REPORT["source_commit"]:
        REPORT["backends"] = previous["backends"]
for backend, (directory, config, arch) in MATRIX.items():
    if sys.argv[1:] and backend not in sys.argv[1:]:
        continue
    folder = ROOT / directory
    env = dict(os.environ)
    env["PATH"] = str(folder) + os.pathsep + env["PATH"]
    previous_checks = REPORT["backends"].get(backend, {}).get("checks", {})
    record = {"build_directory": directory, "configuration": config,
              "gpu_target": arch, "compilation": "passed",
              "checks": previous_checks if network_only else {}, "artifacts": {}}
    record["build_log"] = (f"tmp/rebase-build-{backend}-build.log" if backend == "cpu"
                           else f"tmp/rebase-build-{backend}-{backend}-build.log")
    record["build_script"] = f"build/rebase-build-{backend}.cmd"
    if backend == "sycl":
        record["build_script"] = "build/rebase-build-sycl-host.cmd"
        record["build_log"] = "tmp/rebase-build-sycl-host-build.log"
        record["compilation"] = "native host passed; GPU engine failed"
        record["gpu_engine_build_log"] = "tmp/rebase-build-sycl-sycl-build.log"
        record["gpu_engine_limitations"] = [
            "oneMKL is not installed",
            "gguf_expert_source.cpp uses POSIX unistd.h/open/pread",
            "docs/INTEL_ARC.md states that the engine has no Windows build path",
        ]
    cache = (folder / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace")
    record["compiler"] = next(line.split("=", 1)[1] for line in cache.splitlines()
                              if line.startswith("CMAKE_CXX_COMPILER:"))
    REPORT["backends"][backend] = record
    names = ["strata-host.exe"]
    if backend in ("cuda", "hip"):
        names.append("strata.exe")
    if backend != "sycl":
        names.append("strata_network_backend.dll")
    for name in names:
        path = folder / name
        record["artifacts"][name] = {
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "modified_at_utc": datetime.fromtimestamp(path.stat().st_mtime, timezone.utc).isoformat(),
        }
        if name.endswith(".exe") and not network_only:
            record["checks"][name + " --help"] = check(
                backend + "-" + path.stem + "-help", [str(path), "--help"], env)
    if not network_only:
        record["checks"]["native_host_tests"] = check(
            backend + "-native-host", [sys.executable, "tools/test_native_host.py",
                                       "--exe", str(folder / "strata-host.exe")], env)
    if backend != "sycl":
        network_command = [sys.executable, "tools/test_network_validation.py",
                           "--library", str(folder / "strata_network_backend.dll")]
        if backend == "hip":
            # Python's secure DLL loader ignores PATH; register the SDK directory
            # in this test process, without copying runtime DLLs into a build tree.
            sdk_bin = ROOT / ".rocm-win/Lib/site-packages/_rocm_sdk_devel/bin"
            record["network_test_runtime_directory"] = str(sdk_bin)
            network_command = [sys.executable, "-c",
                "import os,runpy,sys; dll_dir=os.add_dll_directory(sys.argv.pop(1)); "
                "sys.path.insert(0,'tools'); sys.argv[0]='tools/test_network_validation.py'; "
                "runpy.run_path(sys.argv[0],run_name='__main__')",
                str(sdk_bin), "--library", str(folder / "strata_network_backend.dll")]
        record["checks"]["network_validation"] = check(backend + "-network", network_command, env)
    failed |= any(item["exit_code"] for item in record["checks"].values())
REPORT["checks_passed"] = not any(
    item["exit_code"] for record in REPORT["backends"].values()
    for item in record["checks"].values())
REPORT["all_four_gpu_and_host_builds_passed"] = False
report_path.write_text(
    json.dumps(REPORT, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
raise SystemExit(1 if failed else 0)
