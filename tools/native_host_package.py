"""Resources and binary limits for the opt-in Python-free host package."""
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]


def add_resources(stage, build, node):
    shutil.copytree(ROOT / "serve/web", stage / "web", dirs_exist_ok=True)
    shutil.copy2(ROOT / "serve/chat_template.jinja", stage / "chat_template.jinja")
    shutil.copytree(build / "native-text-licenses", stage / "licenses/native-host", dirs_exist_ok=True)
    (stage / "configs").mkdir(exist_ok=True)
    for name in ("cluster-2hosts.json", "cluster-3hosts.json", "cluster-4hosts.example.json"):
        shutil.copy2(ROOT / "configs" / name, stage / "configs" / name)
    (stage / "RUN.txt").write_text(
        "Native host package; model files are not included. Python is not required at runtime.\n"
        "Edit model/library paths in the common plan and copy the same cluster.json to every node.\n"
        "Keep STRATA_NETWORK_KEY in the environment; keep API authentication when serving beyond loopback.\n"
        f"Run: strata-host.exe cluster --config cluster.json --node {node}\n"
        "Other commands: strata-host.exe serve, worker, vision (each accepts --help).\n"
        "The coordinator needs its installed server config and tokenizer.\n"
        "For images, set vision.mmproj and vision.model; omit vision.exe to use the built-in encoder.\n"
        "The built-in encoder uses CPU ggml; third-party GPU/compiler runtime DLLs are separate dependencies.\n",
        encoding="utf-8")


def verify_binaries(stage, programs):
    expected = {name.lower() for name in programs if Path(name).suffix.lower() == ".exe"}
    executables = {p.name.lower() for p in stage.rglob("*.exe")}
    if executables != expected or len(expected) > 2:
        raise ValueError(f"native deployment must contain at most two executables: {sorted(executables)}")
    libraries = {p.name.lower() for p in stage.rglob("*.dll") if p.name.lower().startswith("strata")}
    if libraries != {"strata_network_backend.dll"}:
        raise ValueError(f"native deployment must contain one Strata DLL: {sorted(libraries)}")
    if any(stage.rglob("*.py")):
        raise ValueError("native deployment must not include Python runtime scripts")
