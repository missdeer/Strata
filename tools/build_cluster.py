"""Build and package host16 (oneAPI/CUDA), host11 (portable oneAPI), and host17 (HIP/MSVC ABI)."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/hip"))
from dll_closure import pe_imports

HOSTS = ("host16", "host11", "host17")
PROGRAMS = {
    "host16": ("strata.exe", "strata-device.exe", "strata_network_backend.dll", "strata-network-check.exe"),
    "host11": ("strata_network_backend.dll", "strata-network-check.exe"),
    "host17": ("strata.exe", "strata-device.exe", "strata_network_backend.dll", "strata-network-check.exe"),
}
NATIVE_PROGRAMS = {
    "host16": ("strata.exe", "strata-host.exe", "strata_network_backend.dll"),
    "host11": ("strata-host.exe", "strata_network_backend.dll"),
    "host17": ("strata.exe", "strata-host.exe", "strata_network_backend.dll"),
}
SYSTEM_DLLS = {
    "kernel32.dll", "kernelbase.dll", "ntdll.dll", "advapi32.dll", "ws2_32.dll", "user32.dll", "gdi32.dll",
    "rpcrt4.dll", "ole32.dll", "oleaut32.dll", "shell32.dll", "shlwapi.dll", "secur32.dll", "crypt32.dll",
    "bcrypt.dll", "bcryptprimitives.dll", "version.dll", "winmm.dll", "psapi.dll", "setupapi.dll", "cfgmgr32.dll",
    "shcore.dll", "imm32.dll", "combase.dll", "dbghelp.dll", "dbgcore.dll", "nvcuda.dll", "ucrtbase.dll",
    "winhttp.dll", "windowscodecs.dll", "pdh.dll",
}


def cache_values(build):
    path = build / "CMakeCache.txt"
    if not path.is_file():
        raise ValueError(f"CMake cache is missing: {path}")
    values = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line and not line.startswith(("#", "//")) and ":" in line and "=" in line:
            name, value = line.split("=", 1)
            values[name.split(":", 1)[0]] = value
    return values


def verify_profile(host, cache, archs):
    cuda, hip, portable = ("ON", "OFF", "OFF") if host == "host16" else (
        ("OFF", "OFF", "ON") if host == "host11" else ("OFF", "ON", "ON"))
    for name, expected in (("STRATA_ENABLE_CUDA", cuda), ("STRATA_ENABLE_HIP", hip), ("STRATA_PORTABLE", portable)):
        if cache.get(name) != expected:
            raise ValueError(f"{host}: {name} must be {expected}, got {cache.get(name)}")
    compiler = cache.get("CMAKE_CXX_COMPILER", "")
    if host != "host17" and Path(compiler).name.lower() != "icx-cl.exe":
        raise ValueError(f"{host}: requires Intel icx-cl.exe, got {compiler}")
    if host == "host17" and "clang" not in Path(compiler).name.lower():
        raise ValueError(f"host17: requires ROCm Clang with MSVC ABI, got {compiler}")
    flags = " ".join(cache.get(k, "") for k in ("CMAKE_C_FLAGS", "CMAKE_CXX_FLAGS", "CMAKE_C_FLAGS_RELEASE", "CMAKE_CXX_FLAGS_RELEASE"))
    native_cpu = re.search(r"(?:/QxHost|/Qx[^\s]+|/arch:AVX512|-march=native|-mcpu=native|-xHost)", flags, re.I)
    if host == "host16":
        if "/qxhost" not in flags.lower() or "/o3" not in flags.lower():
            raise ValueError("host16: expected /O3 /QxHost")
    elif native_cpu:
        raise ValueError(f"{host}: nonportable CPU option: {native_cpu[0]}")
    if host == "host11" and "/o3" not in flags.lower():
        raise ValueError("host11: expected /O3")
    if host != "host16" and cache.get("GGML_NATIVE") != "OFF":
        raise ValueError(f"{host}: GGML_NATIVE must be OFF")
    if host != "host16":
        for name in ("GGML_AVX512", "GGML_AVX512_VBMI", "GGML_AVX512_VNNI", "GGML_AVX512_BF16",
                     "GGML_AVX_VNNI", "GGML_AMX_TILE", "GGML_AMX_INT8", "GGML_AMX_BF16"):
            if cache.get(name) == "ON":
                raise ValueError(f"{host}: ggml must stay on its AVX2 baseline; {name} is ON")
    if host == "host17":
        actual = {a for a in cache.get("CMAKE_HIP_ARCHITECTURES", "").split(";") if a}
        if actual != set(archs):
            raise ValueError(f"host17: cached HIP targets {sorted(actual)} differ from requested {archs}")


def index_dlls(directories):
    files = {}
    for folder in directories:
        try:
            if folder.is_dir():
                for path in folder.iterdir():
                    if path.suffix.lower() == ".dll" and path.is_file():
                        files.setdefault(path.name.lower(), path)
        except OSError:
            # PATH can contain inaccessible app aliases; redistributables come from the SDK/redist folders.
            continue
    return files


def copy_runtime(stage, roots, directories):
    candidates = index_dlls([stage, *directories])
    todo, seen, copied = list(roots), set(), []
    while todo:
        image = todo.pop()
        if image.name.lower() in seen:
            continue
        seen.add(image.name.lower())
        for name in pe_imports(image):
            lower = name.lower()
            if lower.startswith(("api-ms-win-", "ext-ms-win-")) or lower in SYSTEM_DLLS:
                continue
            source = candidates.get(lower)
            if source is None:
                raise ValueError(f"missing runtime DLL {name}, imported by {image.name}; add --runtime-dir DIR")
            destination = stage / source.name
            if source.resolve() != destination.resolve():
                shutil.copy2(source, destination)
                copied.append(source.name)
            todo.append(destination)
    return sorted(set(copied))


def runtime_directories(cache, build, extra):
    folders = [build, *extra]
    for name in ("CUDAToolkit_BIN_DIR", "CUDAToolkit_LIBRARY_DIR"):
        if cache.get(name):
            directory = Path(cache[name]); folders += [directory, directory / "x64"]
    if cache.get("CMAKE_CUDA_COMPILER"):
        directory = Path(cache["CMAKE_CUDA_COMPILER"]).parent
        folders += [directory, directory / "x64"]
    compiler = Path(cache.get("CMAKE_CXX_COMPILER", ""))
    for parent in list(compiler.parents)[:4]:
        folders += [parent / "redist/intel64_win/compiler", parent / "bin", parent / "bin/compiler"]
    if os.environ.get("VCToolsRedistDir"):
        redist = Path(os.environ["VCToolsRedistDir"])
        folders += sorted((redist / "x64").glob("Microsoft.VC*.CRT"))
    vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
    if vswhere.is_file():
        result = subprocess.run([str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                                 "-property", "installationPath"], capture_output=True, text=True, check=True)
        if result.stdout.strip():
            folders += sorted((Path(result.stdout.strip()) / "VC/Redist/MSVC").glob("*/x64/Microsoft.VC*.CRT"), reverse=True)
    folders += [Path(p) for p in os.environ.get("PATH", "").split(os.pathsep) if p]
    return folders


def add_scripts(stage, host):
    target = stage / "tools"; target.mkdir(exist_ok=True)
    for name in ("network_worker.py", "gguf_reader.py", "cluster_config.py"):
        shutil.copy2(ROOT / "tools" / name, target / name)
    if host == "host16":
        for name in ("cluster.py", "make_network_config.py", "strata_tokenizer.py"):
            shutil.copy2(ROOT / "tools" / name, target / name)
        shutil.copytree(ROOT / "serve", stage / "serve", ignore=shutil.ignore_patterns("__pycache__", "*.pyc", "test_*.py"))
    for name in ("cluster-2hosts.json", "cluster-3hosts.json", "cluster-4hosts.example.json"):
        directory = stage / "configs"; directory.mkdir(exist_ok=True)
        shutil.copy2(ROOT / "configs" / name, directory / name)
    instructions = ("This is a compiled node package; model files and Python are not included.\n"
        "Copy the same edited cluster.json to every node. Keep STRATA_NETWORK_KEY in the environment.\n"
        "Edit model/library paths for each machine in the common plan.\n"
        + ("Run: python tools/cluster.py --config cluster.json --node host16\n"
           "The coordinator also needs its installed IQ3_S server config and tokenizer.\n" if host == "host16" else
           f"Run: python tools/network_worker.py --cluster-config cluster.json --node {host}\n")
        + ("HIP gfx1103 is experimental; multi-arch compilation does not enable multi-device scheduling.\n" if host == "host17" else ""))
    (stage / "RUN.txt").write_text(instructions, encoding="utf-8")
    if host == "host16":
        (stage / "requirements.txt").write_text("regex\njinja2\n", encoding="utf-8")


def publish(stage, destination):
    parent = destination.parent.resolve()
    if destination.is_symlink() or getattr(destination, "is_junction", lambda: False)() or destination.resolve().parent != parent:
        raise ValueError(f"output directory must not redirect elsewhere: {destination}")
    backup = None
    if destination.exists():
        marker = destination / "CLUSTER_BUILD.json"
        if not marker.is_file() or json.loads(marker.read_text(encoding="utf-8")).get("node") != destination.name:
            raise ValueError(f"refusing to replace a directory not created for this node: {destination}")
        backup = parent / (destination.name + ".previous-" + datetime.now().strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:6])
        destination.rename(backup)
    try:
        stage.rename(destination)
    except BaseException:
        if backup is not None:
            backup.rename(destination)
        raise
    return str(backup) if backup else None


def local_ggml(explicit):
    if explicit:
        path = Path(explicit).resolve()
        if not (path / "ggml/CMakeLists.txt").is_file():
            raise ValueError(f"not a llama.cpp checkout: {path}")
        return path
    for path in (ROOT / "third_party/llama.cpp", ROOT / "build-intel/_deps/strata_llamacpp-src"):
        if (path / "ggml/CMakeLists.txt").is_file():
            return path
    return None


def run_build(host, build, hip_output, args):
    env = dict(os.environ)
    for key in ("CFLAGS", "CXXFLAGS", "CUDAFLAGS", "HIPFLAGS", "STRATA_CLUSTER_BUILD_DIR", "BUILD_DIR", "DIST_DIR"):
        env.pop(key, None)
    env["STRATA_BUILD_JOBS"] = str(args.jobs)
    env["STRATA_CLUSTER_NATIVE"] = "ON" if args.native else "OFF"
    ggml = local_ggml(args.ggml_dir)
    if ggml:
        env["STRATA_GGML_DIR"] = str(ggml)
    if host == "host17":
        script = ROOT / "tools/hip/build_windows.bat"
        env.update(BUILD_DIR=str(build), DIST_DIR=str(hip_output), ROCM_VENV=str(args.rocm_venv),
                   STRATA_HIP_ARCHS=";".join(args.hip_archs), STRATA_ROCM_SKIP_INSTALL="1" if args.no_install_rocm else "0")
        command = ["cmd.exe", "/d", "/c", "call", str(script), "native-network" if args.native else "network"]
    else:
        env["STRATA_CLUSTER_BUILD_DIR"] = str(build)
        command = ["cmd.exe", "/d", "/c", "call", str(ROOT / "tools/build_cluster_intel.bat"), host]
    args.log_dir.mkdir(parents=True, exist_ok=True)
    log = args.log_dir / (host + ".log")
    print(f"[{host}] building; log: {log}", flush=True)
    with log.open("w", encoding="utf-8") as stream:
        result = subprocess.run(command, env=env, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        tail = "\n".join(log.read_text(encoding="utf-8", errors="replace").splitlines()[-18:])
        raise RuntimeError(f"{host}: build exited {result.returncode}; see {log}\n{tail}")


def package(host, build, stage, hip_output, args):
    cache = cache_values(build)
    verify_profile(host, cache, args.hip_archs)
    programs = NATIVE_PROGRAMS[host] if args.native else PROGRAMS[host]
    if args.native and cache.get("STRATA_BUILD_NATIVE_CLUSTER") != "ON":
        raise ValueError("native package requires STRATA_BUILD_NATIVE_CLUSTER=ON")
    if host == "host17":
        source = hip_output / "strata-windows-x64-hip"
        if not source.is_dir():
            raise ValueError("host17: the HIP build did not produce its runtime package")
        source.rename(stage)
        shutil.move(str(hip_output / "strata-windows-x64-hip.zip"), str(stage / "strata-windows-x64-hip.zip"))
    else:
        stage.mkdir()
        for name in programs:
            source = build / name
            if not source.is_file():
                raise ValueError(f"required compiled file is missing: {source}")
            shutil.copy2(source, stage / name)
        copy_runtime(stage, [stage / name for name in programs], runtime_directories(cache, build, args.runtime_dir))
    for name in programs:
        if not (stage / name).is_file():
            raise ValueError(f"{host}: missing program in deployment package: {name}")
    if args.native:
        from native_host_package import add_resources, verify_binaries
        add_resources(stage, build, host)
        verify_binaries(stage, programs)
    else:
        add_scripts(stage, host)
    meta = {"node": host, "built_at_utc": datetime.now(timezone.utc).isoformat(), "build_directory": str(build),
            "compiler": cache.get("CMAKE_CXX_COMPILER"), "c_release_flags": cache.get("CMAKE_C_FLAGS_RELEASE"),
            "cxx_release_flags": cache.get("CMAKE_CXX_FLAGS_RELEASE"), "portable": cache.get("STRATA_PORTABLE") == "ON",
            "backend": "hip" if host == "host17" else "cuda" if host == "host16" else "cpu",
            "hip_archs": args.hip_archs if host == "host17" else [],
            "programs": {name: hashlib.sha256((stage / name).read_bytes()).hexdigest() for name in programs}}
    if args.native:
        meta["native_host"] = True
    (stage / "CLUSTER_BUILD.json").write_text(json.dumps(meta, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--native", action="store_true", help="package strata-host subcommands; no Python runtime or diagnostic executables")
    p.add_argument("--hosts", nargs="+", choices=HOSTS, default=list(HOSTS))
    p.add_argument("--build-root", type=Path, default=ROOT / "build-cluster")
    p.add_argument("--out-root", type=Path, default=ROOT / "dist/cluster")
    p.add_argument("--build-dir", action="append", default=[], metavar="HOST=DIR", help="reuse an existing build for one node")
    p.add_argument("--jobs", type=int, default=6)
    p.add_argument("--hip-archs", nargs="+", default=["gfx1102", "gfx1103"])
    p.add_argument("--ggml-dir", help="pinned local llama.cpp checkout; otherwise reuse local source if available")
    p.add_argument("--rocm-venv", type=Path, default=ROOT / ".rocm-win")
    p.add_argument("--runtime-dir", action="append", type=Path, default=[], help="extra folder containing redistributable DLLs")
    p.add_argument("--no-install-rocm", action="store_true", help="fail host17 if its ROCm SDK is missing; never download it")
    p.add_argument("--dry-run", action="store_true", help="print the plan; no compilation, downloads or writes")
    p.add_argument("--package-only", action="store_true", help="package existing host16/host11 builds without compiling")
    args = p.parse_args()
    if not 1 <= args.jobs <= 128 or len(set(args.hosts)) != len(args.hosts):
        p.error("jobs must be 1..128 and hosts must be distinct")
    if not args.hip_archs or len(set(args.hip_archs)) != len(args.hip_archs) or any(re.fullmatch(r"gfx[0-9a-f]+", a) is None for a in args.hip_archs):
        p.error("provide distinct gfx architecture names")
    if args.package_only and "host17" in args.hosts:
        p.error("--package-only supports host16/host11; host17 is packaged by its HIP build")
    overrides = {}
    for value in args.build_dir:
        host, separator, path = value.partition("=")
        if not separator or host not in args.hosts or host in overrides or not path:
            p.error("--build-dir must specify a selected host once as HOST=DIR")
        overrides[host] = Path(path).resolve()
    args.build_root, args.out_root, args.rocm_venv = args.build_root.resolve(), args.out_root.resolve(), args.rocm_venv.resolve()
    builds = {host: overrides.get(host, args.build_root / host).resolve() for host in args.hosts}
    destinations = {host: args.out_root / host for host in args.hosts}
    if any(path.is_symlink() or getattr(path, "is_junction", lambda: False)() or path.resolve().parent != args.out_root
           for path in destinations.values()):
        p.error("node output directories must stay directly inside --out-root without symlinks or junctions")
    paths = list(builds.values()) + list(destinations.values())
    if any(a == b or a.is_relative_to(b) or b.is_relative_to(a) for i, a in enumerate(paths) for b in paths[i + 1:]):
        p.error("each node's build and output directories must be independent and must not contain one another")
    args.log_dir = args.out_root / "_logs" / (datetime.now().strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:6])
    for host in args.hosts:
        compiler = "oneAPI /O3 /QxHost + CUDA sm_89" if host == "host16" else (
            "oneAPI /O3, AVX2, no /QxHost" if host == "host11" else "ROCm Clang + MSVC ABI, " + ",".join(args.hip_archs))
        print(f"{host}: {compiler}\n  build: {builds[host]}\n  output: {destinations[host]}", flush=True)
    if args.dry_run:
        print("Dry run: no files written, tools invoked, or packages downloaded.")
        return 0
    if os.name != "nt":
        p.error("this build driver requires Windows; --dry-run is available elsewhere")
    args.out_root.mkdir(parents=True, exist_ok=True)
    results = {}
    for host in args.hosts:
        try:
            with tempfile.TemporaryDirectory(prefix=".cluster-stage-", dir=args.out_root) as temporary:
                temp = Path(temporary)
                hip_output = temp / "hip-package"
                if not args.package_only:
                    run_build(host, builds[host], hip_output, args)
                stage = temp / host
                package(host, builds[host], stage, hip_output, args)
                backup = publish(stage, destinations[host])
                results[host] = {"status": "success", "output": str(destinations[host]), "previous_output": backup}
                print(f"[{host}] ready: {destinations[host]}", flush=True)
        except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
            results[host] = {"status": "failed", "error": str(error)}
            print(f"[{host}] FAILED: {error}", file=sys.stderr, flush=True)
    (args.out_root / "build-summary.json").write_text(json.dumps(results, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return 1 if any(r["status"] != "success" for r in results.values()) else 0


if __name__ == "__main__":
    raise SystemExit(main())
