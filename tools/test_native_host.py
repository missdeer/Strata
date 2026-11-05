"""Integration checks for the single deployable host and package boundaries."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from native_host_package import verify_binaries

parser = argparse.ArgumentParser()
parser.add_argument("--exe", type=Path, required=True)
ARGS = parser.parse_args()


class HostTests(unittest.TestCase):
    def run_host(self, *args, exe=None):
        return subprocess.run([str(exe or ARGS.exe.resolve()), *map(str, args)],
                              capture_output=True, encoding="utf-8", errors="replace", timeout=20)

    def test_usage_and_unknown_commands_do_not_start_a_service(self):
        self.assertNotEqual(self.run_host().returncode, 0)
        self.assertNotEqual(self.run_host("missing-command").returncode, 0)
        for command in (None, "cluster", "worker", "serve", "vision"):
            result = self.run_host(*([command] if command else []), "--help")
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_renamed_host_validates_without_sibling_role_executables(self):
        with tempfile.TemporaryDirectory(prefix="host-模型-") as folder:
            host = Path(folder) / ("宿主 with spaces" + ARGS.exe.suffix)
            shutil.copy2(ARGS.exe, host)
            plan = Path(__file__).resolve().parents[1] / "configs/cluster-2hosts.json"
            cluster = self.run_host("cluster", "--config", plan, "--validate", exe=host)
            worker = self.run_host("worker", "--cluster-config", plan, "--validate", exe=host)
            self.assertEqual(cluster.returncode, 0, cluster.stderr)
            self.assertEqual(worker.returncode, 0, worker.stderr)
            self.assertEqual(json.loads(cluster.stdout), json.loads(worker.stdout))

    def test_encoder_is_linked_and_rejects_missing_model(self):
        with tempfile.TemporaryDirectory() as folder:
            result = self.run_host("vision", "--mmproj", Path(folder) / "missing-projector.gguf",
                                   "--model", Path(folder) / "missing-model.gguf")
            self.assertNotEqual(result.returncode, 0)
            self.assertNotIn("unknown command", result.stderr)
            self.assertNotIn("READY", result.stdout)
            self.assertIn("missing-model.gguf", result.stderr)
            gpu = self.run_host("vision", "--mmproj", "unused", "--model", "unused", "--gpu")
            self.assertNotEqual(gpu.returncode, 0)
            self.assertIn("only a CPU backend", gpu.stdout)

    def test_package_rejects_extra_executables_project_dlls_and_python(self):
        with tempfile.TemporaryDirectory() as folder:
            stage = Path(folder)
            programs = ("strata.exe", "strata-host.exe", "strata_network_backend.dll")
            for name in programs:
                (stage / name).touch()
            # Third-party runtime DLLs do not count as Strata DLL targets.
            (stage / "amdhip64_7.dll").touch()
            verify_binaries(stage, programs)
            for name in ("strata-server.exe", "strata-network-test-backend.dll", "network_worker.py"):
                extra = stage / name
                extra.touch()
                with self.assertRaises(ValueError):
                    verify_binaries(stage, programs)
                extra.unlink()


if __name__ == "__main__":
    unittest.main(argv=["test_native_host"], verbosity=2)
