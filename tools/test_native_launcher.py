"""Native stage-2 launcher parity and process-tree tests; no model/download/GPU needed."""
import argparse
import ctypes
import json
import os
from pathlib import Path
import signal
import shutil
import socket
import struct
import subprocess
import tempfile
import time
import unittest

from cluster_config import ClusterConfig
from make_network_config import make_config

ROOT = Path(__file__).resolve().parents[1]
KEY = "launcher-test-key-模型"
SETTINGS = None


def alive(pid):
    if os.name == "nt":
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.restype = ctypes.c_void_p
        kernel.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
        kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        handle = kernel.OpenProcess(0x100000, False, pid)
        if not handle:
            return False
        try:
            return kernel.WaitForSingleObject(handle, 0) == 258
        finally:
            kernel.CloseHandle(handle)
    try:
        os.kill(pid, 0)
        status = Path(f"/proc/{pid}/stat").read_text()
        return status.split(")", 1)[1].split()[0] != "Z"
    except (ProcessLookupError, FileNotFoundError):
        return False


def until(check, seconds=15):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if check():
            return
        time.sleep(.025)
    raise AssertionError("timed out waiting for native process state")


class LauncherTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="launcher-模型😀-")
        self.folder = Path(self.temp.name)
        self.plan_path = self.folder / "共享.json"
        self.source_path = self.folder / "服务.json"
        self.record = self.folder / "record.json"
        self.role = self.folder / ("native role 模型😀" + SETTINGS.role.suffix)
        shutil.copyfile(SETTINGS.role, self.role)
        self.data = json.loads((ROOT / "configs/cluster-3hosts.json").read_text())
        self.data["nodes"][0].update(server_config=str(self.source_path), engine=str(self.folder / '引擎 with spaces.exe'))
        self.source = dict(args=["--native", '模型😀"quoted"\\', "--network-workers", "stale", "--batch", "8",
                                 "--network-reduce", "--mmap-experts", "--keep", "", "--other", 'ends with slash\\'],
                           host="0.0.0.0", port=19001, gpu=[0, 1], api_key="source-secret",
                           model_name="model", batch=8, layer_split=16, test_record=str(self.record))
        self.env = dict(os.environ, STRATA_NETWORK_KEY=KEY, STRATA_API_KEY="environment-secret")
        self.addCleanup(self.temp.cleanup)
        self.save()

    def save(self):
        self.plan_path.write_text(json.dumps(self.data, ensure_ascii=False), encoding="utf-8-sig")
        self.source_path.write_text(json.dumps(self.source, ensure_ascii=False), encoding="utf-8-sig")

    def command(self, *args, env=None):
        return subprocess.run([str(SETTINGS.launcher), "cluster", "--config", str(self.plan_path), *map(str, args)],
                              env=env or self.env, cwd=self.folder, capture_output=True, encoding="utf-8", timeout=20)

    def export(self, *args):
        out = self.folder / "generated.json"
        result = self.command("--node", "host16", "--export-config", out, *args)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(out.read_text(encoding="utf-8"))

    def test_validate_shipped_plans(self):
        for name in ("cluster-2hosts.json", "cluster-3hosts.json", "cluster-4hosts.example.json"):
            self.data = json.loads((ROOT / "configs" / name).read_text())
            self.data["cluster_id"] = "集群😀"
            self.data["nodes"].reverse()
            self.save()
            plan = ClusterConfig(self.data)
            result = self.command("--validate", env=dict(self.env, STRATA_NETWORK_KEY=""))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout), dict(config_sha256=plan.digest, assignment=plan.assignment, endpoints=plan.endpoints))
            self.assertNotIn(KEY, result.stdout + result.stderr)

    def expected(self, prefill=None, engine=None, no_key=False):
        plan = ClusterConfig(self.data)
        node = plan.coordinator
        cfg = make_config(self.source, plan.endpoints, node["context"], engine or node["engine"],
                          node.get("network_reduce", False), node.get("network_local_ram_mib", 0))
        cfg["log"] = str(self.folder / "logs" / "cluster-host16.log")
        cfg["args"] += ["--prefill", prefill or str(node["prefill"]), "--network-assignment", plan.assignment,
                        "--network-config-id", plan.digest, "--network-timeout-ms", str(plan.data["scheduler"]["timeout_ms"])]
        if no_key:
            cfg.pop("api_key", None)
        # Compare paths after platform-specific separator normalization.
        for key in ("exe", "log"):
            cfg[key] = str(Path(cfg[key]))
        return cfg

    def test_config_matches_python_without_changing_inputs(self):
        before = (self.plan_path.read_bytes(), self.source_path.read_bytes())
        actual = self.export()
        for key in ("exe", "log"):
            actual[key] = str(Path(actual[key]))
        self.assertEqual(actual, self.expected())
        self.assertEqual(before, (self.plan_path.read_bytes(), self.source_path.read_bytes()))
        self.assertNotIn(KEY, json.dumps(actual))

    def test_overrides_reduce_and_ram(self):
        self.data["nodes"][0].update(network_reduce=True, network_local_ram_mib=4096)
        self.save()
        engine = self.folder / "覆盖.exe"
        actual = self.export("--exe", engine, "--prefill", "auto:8192", "--no-api-key")
        for key in ("exe", "log"):
            actual[key] = str(Path(actual[key]))
        self.assertEqual(actual, self.expected("auto:8192", engine, True))

    def test_exports_never_overwrite_inputs_or_existing_files(self):
        for target in (self.plan_path, self.source_path, self.folder / "existing.json"):
            if not target.exists():
                target.write_text("untouched")
            before = target.read_bytes()
            result = self.command("--node", "host16", "--export-config", target)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(target.read_bytes(), before)

    def test_invalid_json_and_source(self):
        self.plan_path.write_text('{"revision":1,"revision":2}')
        self.assertIn("duplicate JSON", self.command("--validate").stderr)
        for source in ({"args": ["--native", "m", "--batch"]}, {"args": ["--native", 7]},
                       {"args": ["--native", "m"], "port": True}, {"args": ["--native", "m"], "port": 65536},
                       {"args": ["--native", "m"], "port": 0}, {"args": ["--native", "m"], "port": 2**64-1},
                       {"args": ["model"]}):
            self.source = source
            self.save()
            result = self.command("--node", "host16", "--export-config", self.folder / "bad.json")
            self.assertNotEqual(result.returncode, 0, source)
            self.assertFalse((self.folder / "bad.json").exists())

    def test_bad_cli_offline_and_worker_overrides(self):
        cases = [[], ["--node", "missing"], ["--node", "host16", "--prefill", "7"],
                 ["--node", "host11", "--exe", "x"], ["--node", "host11", "--no-api-key"],
                 ["--node", "host11", "--export-config", "x"], ["--validate", "--validate"], ["--unknown"]]
        for args in cases:
            self.assertNotEqual(self.command(*args).returncode, 0, args)
        self.data["nodes"][2]["online"] = False
        self.data["nodes"][1]["expert_ranges"] = [[0, 511]]
        self.save()
        self.assertIn("offline", self.command("--node", "host17").stderr)

    def test_network_key_length_and_missing_api(self):
        for key in ("", "x" * 15, "x" * 257):
            result = self.command("--node", "host16", env=dict(self.env, STRATA_NETWORK_KEY=key))
            self.assertIn("16..256", result.stderr)
        # The default API is this host; exercise a missing explicit override.
        self.assertIn("native API executable unavailable", self.command("--node", "host16", "--api-exe", self.folder / "missing-api.exe").stderr)
        self.assertFalse(self.record.exists())

    def test_json_error_does_not_log_private_values(self):
        self.source_path.write_text('{"api_key":"private-do-not-log", BROKEN}', encoding="utf-8")
        result = self.command("--node", "host16")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("private-do-not-log", result.stderr)

    def test_api_spawn_failure_removes_runtime_directory(self):
        invalid = self.folder / "invalid.exe"
        invalid.write_text("not an executable")
        before = set(Path(tempfile.gettempdir()).glob("strata-cluster-*"))
        result = self.command("--node", "host16", "--api-exe", invalid)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(before, set(Path(tempfile.gettempdir()).glob("strata-cluster-*")))

    def test_real_worker_with_no_python_in_path(self):
        import network_worker as W
        from test_network_experts import body
        library = SETTINGS.library
        backend = W.NativeBackend(library, workers=1)
        self.addCleanup(backend.close)
        blob_bytes = backend.lib.strata_network_format_bytes(22, 42)
        pack = self.folder / "pack"; pack.mkdir()
        (pack / "native_experts.txt").write_text(f"# strata native experts v3\n0 22 42 0 {blob_bytes}\n")
        with (pack / "experts.bin").open("wb") as file:
            if os.name == "nt":
                import msvcrt
                kernel = ctypes.WinDLL("kernel32", use_last_error=True)
                kernel.DeviceIoControl.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32,
                                                  ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_void_p]
                returned = ctypes.c_uint32()
                if not kernel.DeviceIoControl(msvcrt.get_osfhandle(file.fileno()), 0x900c4, None, 0, None, 0,
                                              ctypes.byref(returned), None):
                    raise RuntimeError("cannot mark test pack sparse")
            file.truncate(blob_bytes * 512)
        source = W.ExpertFiles(pack, cache_mib=64)
        self.addCleanup(source.close)
        self.data["model"]["layers"] = 1
        self.data["nodes"][0]["host"] = "127.0.0.1"
        self.data["nodes"][2]["online"] = False
        with socket.socket() as probe:
            probe.bind(("127.0.0.2", 0))
            port = probe.getsockname()[1]
        self.data["nodes"][1].update(host="127.0.0.2", port=port, backend="cpu", pack=str(pack), native=str(pack / "unused.gguf"),
                                    library=str(library), workers=1, ram_cache_mib=64, expert_ranges=[[0, 511]])
        self.save()
        plan = ClusterConfig(self.data)
        env = dict(self.env, PATH=str(Path(os.environ["SystemRoot"]) / "System32") if os.name == "nt" else "/usr/bin:/bin")
        log_path = self.folder / "worker.log"
        with log_path.open("wb") as log:
            process = subprocess.Popen([str(SETTINGS.launcher), "cluster", "--config", str(self.plan_path), "--node", "host11"],
                                       env=env, cwd=self.folder, stdout=log, stderr=log,
                                       creationflags=subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0)
            try:
                until(lambda: "worker ready:" in log_path.read_text(encoding="utf-8", errors="replace"))
                with socket.create_connection(("127.0.0.2", port), timeout=3) as conn:
                    hello = struct.pack("<QIIIII", source.fingerprint(), 1, 512, 0, 1, W.H)
                    hello += plan.digest.encode() + plan.assignment.encode() + KEY.encode()
                    W.send(conn, 0, 3, hello, 3)
                    self.assertEqual(W.frame(conn, 3), (0, 131, b""))
                    W.send(conn, 1, 2, body(((0, 2),)), 3)
                    self.assertEqual(W.frame(conn, 3), (1, 130, bytes(W.H * 4)))
                process.send_signal(signal.CTRL_BREAK_EVENT if os.name == "nt" else signal.SIGTERM)
                process.wait(timeout=20)
                # Immediate rebinding also checks that the contained listener ended.
                with socket.socket() as probe:
                    probe.bind(("127.0.0.2", port))
                self.assertNotIn(KEY, log_path.read_text(encoding="utf-8"))
            finally:
                if process.poll() is None:
                    process.kill(); process.wait(timeout=20)

    def start_role(self, **settings):
        self.source.update(settings)
        self.save()
        process = subprocess.Popen([str(SETTINGS.launcher), "cluster", "--config", str(self.plan_path), "--node", "host16",
                                    "--api-exe", str(self.role), "--no-api-key"], env=self.env, cwd=self.folder,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                   creationflags=subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0)
        def cleanup():
            if process.poll() is None:
                process.kill()
            process.wait(timeout=20)
        self.addCleanup(cleanup)
        until(lambda: self.record.exists() and self.record.stat().st_size > 0)
        # Wait until the complete record is flushed, rather than observing its first write.
        def load():
            try:
                self.snapshot = json.loads(self.record.read_text(encoding="utf-8"))
                return True
            except json.JSONDecodeError:
                return False
        until(load)
        return process

    def test_native_child_argv_key_cwd_exit_and_cleanup(self):
        process = self.start_role(test_exit=7)
        self.assertEqual(process.wait(timeout=20), 7)
        snapshot = self.snapshot
        self.assertFalse(snapshot["test_api_key_present"])
        self.assertNotIn("api_key", snapshot)
        self.assertEqual(snapshot["args"], self.expected(no_key=True)["args"])
        self.assertEqual(snapshot["test_argv"][-2:], ["--port", "19001"])
        self.assertEqual(snapshot["test_argv"][1], "serve")
        self.assertEqual(Path(snapshot["test_cwd"]), self.folder)
        self.assertFalse(Path(snapshot["test_runtime_path"]).parent.exists())

    def process_tree(self, abrupt=False, wait=True):
        process = self.start_role(test_wait=wait, test_descendant=True)
        leaf = Path(str(self.record) + ".leaf")
        if wait:
            until(leaf.exists)
            until(lambda: leaf.stat().st_size > 0)
            descendant = int(leaf.read_text())
            self.assertTrue(alive(descendant))
            if abrupt:
                process.kill()
            elif os.name == "nt":
                process.send_signal(signal.CTRL_BREAK_EVENT)
            else:
                process.send_signal(signal.SIGTERM)
            process.wait(timeout=20)
            until(lambda: not alive(descendant))
        else:
            process.wait(timeout=20)
            if leaf.exists() and leaf.stat().st_size:
                until(lambda: not alive(int(leaf.read_text())))
        until(lambda: not alive(self.snapshot["test_pid"]))
        runtime = Path(self.snapshot["test_runtime_path"]).parent
        if abrupt:
            # The runtime remains owner-only after uncatchable termination.
            # Remove this test artifact once the contained processes are gone.
            shutil.rmtree(runtime, ignore_errors=True)
        else:
            self.assertFalse(runtime.exists())

    def test_graceful_stop_removes_descendants(self):
        self.process_tree()

    def test_stop_timeout_kills_unresponsive_child(self):
        self.source["test_ignore_stop"] = True
        self.process_tree()

    def test_killed_launcher_removes_descendants(self):
        self.process_tree(abrupt=True)

    def test_child_exit_removes_descendants(self):
        self.process_tree(wait=False)

    @unittest.skipUnless(os.name != "nt", "POSIX permissions")
    def test_private_config_permissions(self):
        self.export()
        self.assertEqual((self.folder / "generated.json").stat().st_mode & 0o777, 0o600)

    @unittest.skipUnless(os.name == "nt", "Windows private ACL")
    def test_private_config_acl(self):
        self.export()
        security = ctypes.WinDLL("advapi32", use_last_error=True)
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        pointer = ctypes.POINTER(ctypes.c_void_p)
        security.GetNamedSecurityInfoW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32,
                                                  pointer, pointer, pointer, pointer, pointer]
        security.ConvertSecurityDescriptorToStringSecurityDescriptorW.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                                                                 ctypes.c_uint32, pointer, ctypes.c_void_p]
        kernel.LocalFree.argtypes = [ctypes.c_void_p]
        descriptor, sddl_pointer = ctypes.c_void_p(), ctypes.c_void_p()
        self.assertEqual(security.GetNamedSecurityInfoW(str(self.folder / "generated.json"), 1, 4,
                                                        None, None, None, None, ctypes.byref(descriptor)), 0)
        try:
            self.assertTrue(security.ConvertSecurityDescriptorToStringSecurityDescriptorW(descriptor, 1, 4,
                                                                                          ctypes.byref(sddl_pointer), None))
            sddl = ctypes.wstring_at(sddl_pointer)
            self.assertIn("D:P", sddl)
            self.assertEqual(sddl.count("(A;"), 1, sddl)
            self.assertIn(";;;OW)", sddl)
        finally:
            kernel.LocalFree(sddl_pointer)
            kernel.LocalFree(descriptor)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--launcher", type=Path, required=True)
    parser.add_argument("--role", type=Path, required=True)
    parser.add_argument("--library", type=Path, required=True)
    SETTINGS = parser.parse_args()
    SETTINGS.launcher = SETTINGS.launcher.resolve()
    SETTINGS.role = SETTINGS.role.resolve()
    SETTINGS.library = SETTINGS.library.resolve()
    if not unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(LauncherTests)).wasSuccessful():
        raise SystemExit(1)
