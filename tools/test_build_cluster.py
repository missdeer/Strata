"""Check deployment isolation, CPU portability guards, DLL closure and safe output replacement."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import build_cluster as B


def cpu_cache():
    return {"STRATA_ENABLE_CUDA": "OFF", "STRATA_ENABLE_HIP": "OFF", "STRATA_PORTABLE": "ON",
            "CMAKE_CXX_COMPILER": "C:/Intel/icx-cl.exe", "CMAKE_CXX_FLAGS_RELEASE": "/O3 /DNDEBUG /fp:precise",
            "GGML_NATIVE": "OFF"}


class DeploymentTests(unittest.TestCase):
    def test_host11_rejects_host_specific_isa(self):
        for option in ("/QxHost", "/QxCORE-AVX512", "/arch:AVX512", "-march=native"):
            cache = cpu_cache(); cache["CMAKE_CXX_FLAGS"] = option
            with self.assertRaisesRegex(ValueError, "nonportable"):
                B.verify_profile("host11", cache, [])

    def test_host11_rejects_gpu_backend(self):
        cache = cpu_cache(); cache["STRATA_ENABLE_CUDA"] = "ON"
        with self.assertRaisesRegex(ValueError, "STRATA_ENABLE_CUDA"):
            B.verify_profile("host11", cache, [])

    def test_host11_rejects_native_ggml(self):
        cache = cpu_cache(); cache["GGML_NATIVE"] = "ON"
        with self.assertRaisesRegex(ValueError, "GGML_NATIVE"):
            B.verify_profile("host11", cache, [])

    def test_host11_rejects_cached_ggml_avx512(self):
        cache = cpu_cache(); cache["GGML_AVX512"] = "ON"
        with self.assertRaisesRegex(ValueError, "GGML_AVX512"):
            B.verify_profile("host11", cache, [])

    def test_host17_rejects_stale_single_gpu_cache(self):
        cache = cpu_cache()
        cache.update(STRATA_ENABLE_HIP="ON", CMAKE_CXX_COMPILER="C:/ROCm/clang++.exe",
                     CMAKE_HIP_ARCHITECTURES="gfx1102", CMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG")
        with self.assertRaisesRegex(ValueError, "HIP targets"):
            B.verify_profile("host17", cache, ["gfx1102", "gfx1103"])

    def test_runtime_copy_follows_recursive_dependencies(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); sdk = root / "sdk"; sdk.mkdir(); stage = root / "stage"; stage.mkdir()
            program = stage / "program.exe"; program.write_bytes(b"program")
            (sdk / "a.dll").write_bytes(b"a"); (sdk / "b.dll").write_bytes(b"b")
            imports = {"program.exe": ["A.dll", "KERNEL32.dll", "WINHTTP.dll", "pdh.dll"],
                       "a.dll": ["b.dll"], "b.dll": ["WS2_32.dll"]}
            with patch.object(B, "pe_imports", lambda image: imports[image.name]):
                copied = B.copy_runtime(stage, [program], [sdk])
            self.assertEqual(copied, ["a.dll", "b.dll"])
            self.assertEqual((stage / "b.dll").read_bytes(), b"b")
            self.assertFalse((stage / "KERNEL32.dll").exists())

    def test_missing_runtime_aborts_package(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); program = root / "program.exe"; program.write_bytes(b"program")
            with patch.object(B, "pe_imports", return_value=["missing_math.dll"]):
                with self.assertRaisesRegex(ValueError, "missing_math.dll"):
                    B.copy_runtime(root, [program], [])

    def test_unmanaged_output_is_preserved(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); stage = root / "stage"; stage.mkdir(); dest = root / "host11"; dest.mkdir()
            (dest / "user.txt").write_text("keep")
            with self.assertRaisesRegex(ValueError, "not created"):
                B.publish(stage, dest)
            self.assertEqual((dest / "user.txt").read_text(), "keep")

    def test_rebuild_replaces_only_its_own_node(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); stage = root / "stage"; stage.mkdir(); dest = root / "host11"; dest.mkdir()
            (dest / "CLUSTER_BUILD.json").write_text(json.dumps({"node": "host11"}))
            (dest / "old.dll").write_text("old"); (stage / "new.dll").write_text("new")
            other = root / "host16"; other.mkdir(); (other / "main.exe").write_text("keep")
            backup = Path(B.publish(stage, dest))
            self.assertEqual((backup / "old.dll").read_text(), "old")
            self.assertFalse((dest / "old.dll").exists())
            self.assertEqual((other / "main.exe").read_text(), "keep")

    def test_publish_failure_restores_previous_package(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); stage = root / "stage"; stage.mkdir(); dest = root / "host11"; dest.mkdir()
            (dest / "CLUSTER_BUILD.json").write_text(json.dumps({"node": "host11"}))
            (dest / "old.dll").write_text("old")
            rename = Path.rename
            def fail_publish(path, target):
                if path == stage:
                    raise OSError("simulated file lock")
                return rename(path, target)
            with patch.object(Path, "rename", fail_publish):
                with self.assertRaisesRegex(OSError, "file lock"):
                    B.publish(stage, dest)
            self.assertEqual((dest / "old.dll").read_text(), "old")

    def test_dry_run_has_no_side_effects(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); output = root / "out"; build = root / "build"
            result = subprocess.run([sys.executable, str(B.ROOT / "tools/build_cluster.py"), "--dry-run",
                "--out-root", str(output), "--build-root", str(build)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("gfx1102,gfx1103", result.stdout)
            self.assertFalse(output.exists()); self.assertFalse(build.exists())

    def test_overlapping_build_and_output_are_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            result = subprocess.run([sys.executable, str(B.ROOT / "tools/build_cluster.py"), "--dry-run",
                "--hosts", "host11", "--build-root", folder, "--out-root", folder], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("independent", result.stderr)

    @unittest.skipUnless(os.name == "nt", "Windows batch toolchain preflight")
    def test_no_install_rocm_fails_without_creating_sdk_or_package(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); sdk = root / "missing-sdk"; output = root / "out"
            result = subprocess.run([sys.executable, str(B.ROOT / "tools/build_cluster.py"), "--hosts", "host17",
                "--build-root", str(root / "build"), "--out-root", str(output), "--rocm-venv", str(sdk),
                "--no-install-rocm"], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("installation is disabled", result.stderr)
            self.assertFalse(sdk.exists())
            self.assertFalse((output / "host17").exists())
            summary = json.loads((output / "build-summary.json").read_text(encoding="utf-8"))
            self.assertEqual(summary["host17"]["status"], "failed")


if __name__ == "__main__":
    unittest.main(verbosity=2)
