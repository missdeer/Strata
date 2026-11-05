"""Expert preload tests: complete assignment residency, no post-load disk reads and readiness ordering."""
from collections import OrderedDict
from contextlib import redirect_stdout
import ctypes as C
import io
import mmap
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

import network_worker as W
from cluster_config import ClusterConfig
from test_cluster_config import base


class PreloadTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.source = object.__new__(W.ExpertFiles)
        s = self.source
        s.experts = 4
        s.layers = [(18, 20, 0, 16, 0, 0, []), (21, 20, 64, 24, 0, 0, [])]
        s.cache, s.cache_bytes, s.budget = OrderedDict(), 0, 160
        s.roles = []
        s.bin = Path(self.temp.name) / "experts.bin"
        self.bytes = {(l, e): bytes([l * 10 + e + 1]) * layer[3]
                      for l, layer in enumerate(s.layers) for e in range(s.experts)}
        s.bin.write_bytes(b"".join(self.bytes.values()))
        f = s.bin.open("rb")
        s.files = {s.bin: f}
        s.maps = {s.bin: mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)}
        self.memory = patch.object(W, "available_memory_bytes", return_value=(16 << 30, 16 << 30))
        self.memory.start()

    def tearDown(self):
        self.memory.stop()
        self.source.close()
        self.temp.cleanup()

    def test_full_preload_remains_cached_without_disk_reads(self):
        s = self.source
        messages = []
        self.assertEqual(s.preload(0, 1, progress=messages.append), (8, 160))
        self.assertEqual(s.cache_bytes, 160)
        with patch.object(s, "blob", side_effect=AssertionError("unexpected disk read")):
            for key, expected in self.bytes.items():
                buf = s.pointer(*key)
                self.assertEqual(C.string_at(buf, C.sizeof(buf)), expected)
        self.assertIn("preload complete", messages[-1])

    def test_preloads_only_explicitly_owned_experts(self):
        s = self.source; s.budget = 80
        self.assertEqual(s.preload(0, 2, [0, 1, 1, 0], progress=lambda _: None), (4, 80))
        self.assertEqual(set(s.cache), {(0, 0), (0, 3), (1, 0), (1, 3)})

    def test_legacy_modulo_ownership(self):
        s = self.source; s.budget = 80
        s.preload(1, 2, progress=lambda _: None)
        self.assertEqual(set(s.cache), {(0, 1), (0, 3), (1, 1), (1, 3)})

    def test_insufficient_cache_is_rejected_before_reading(self):
        s = self.source; s.budget = 159
        with patch.object(s, "blob", side_effect=AssertionError("should not read")):
            with self.assertRaisesRegex(ValueError, "RAM cache budget"):
                s.preload(0, 1)
        self.assertEqual(s.cache_bytes, 0)

    def test_insufficient_available_ram_or_commit_is_rejected(self):
        for available in ((100, 10000), (10000, 100)):
            with patch.object(W, "available_memory_bytes", return_value=available), \
                    patch.object(self.source, "blob", side_effect=AssertionError("should not read")):
                with self.assertRaisesRegex(ValueError, "available RAM"):
                    self.source.preload(0, 1, headroom_bytes=0)
        self.assertFalse(self.source.cache)

    def test_loading_failure_releases_partial_buffers(self):
        s = self.source
        original = s.blob
        def read(l, e):
            if (l, e) == (0, 2):
                raise MemoryError("allocation failed")
            return original(l, e)
        with patch.object(s, "blob", side_effect=read):
            with self.assertRaisesRegex(MemoryError, "allocation failed"):
                s.preload(0, 1, progress=lambda _: None)
        self.assertEqual(s.cache_bytes, 0)
        self.assertFalse(s.cache)

    def test_empty_or_invalid_assignment_is_rejected(self):
        for index, count, owners in ((0, 2, [1, 1, 1, 1]), (0, 2, [0, 0]), (0, 2, [0, 1, 2, 1])):
            with self.assertRaises(ValueError):
                self.source.preload(index, count, owners)

    def test_preloaded_buffers_are_reused(self):
        s = self.source
        first = s.pointer(0, 0)
        s.preload(0, 1, progress=lambda _: None)
        self.assertIs(s.pointer(0, 0), first)

    def test_listener_opens_only_after_full_preload(self):
        s = self.source
        args = ["network_worker.py", "--pack", "test", "--library", "test.dll", "--index", "0", "--count", "1",
                "--api-key", "preload-test-secret", "--preload-experts"]
        with patch.object(sys, "argv", args), patch.object(W, "ExpertFiles", return_value=s), \
                patch.object(W, "NativeBackend"), patch.object(W.socket, "socket") as socket_factory, redirect_stdout(io.StringIO()):
            listener = socket_factory.return_value.__enter__.return_value
            def bound(*_):
                self.assertEqual(s.cache_bytes, 160)
                self.assertEqual(set(s.cache), set(self.bytes))
            listener.bind.side_effect = bound
            listener.accept.side_effect = KeyboardInterrupt
            W.main()
            listener.bind.assert_called_once()
            listener.listen.assert_called_once()

    def test_configuration_preload_flag_is_strict_and_affects_identity(self):
        data = base(); original = ClusterConfig(data).digest
        data["nodes"][1]["preload_experts"] = True
        self.assertNotEqual(original, ClusterConfig(data).digest)
        data["nodes"][1]["preload_experts"] = "yes"
        with self.assertRaisesRegex(ValueError, "preload_experts"):
            ClusterConfig(data)


if __name__ == "__main__":
    unittest.main(verbosity=2)
