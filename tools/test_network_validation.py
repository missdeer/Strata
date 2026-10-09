"""Native wire validation, legacy DLL fallback and reusable result-buffer checks."""
import argparse
import ctypes as C
from pathlib import Path
import struct
import unittest
from unittest.mock import patch

import network_worker as W


class ValidationTests(unittest.TestCase):
    library = None

    def setUp(self):
        self.backend = W.NativeBackend(self.library, workers=1)
        self.assertIsNotNone(self.backend._finite, 'test requires the new native validator')

    def tearDown(self):
        self.backend.close()

    def test_all_finite_ieee_patterns(self):
        # Both signed zeros, subnormals and largest finite values are valid.
        words = [0, 0x80000000, 1, 0x80000001, 0x7f7fffff, 0xff7fffff]
        data = struct.pack('<6I', *words)
        with patch.object(W, 'all_finite', side_effect=AssertionError('Python float scan used')):
            self.assertTrue(self.backend.all_finite(data))
            self.assertTrue(self.backend.all_finite(b''))
        for bad in (0x7f800000, 0xff800000, 0x7fc00000, 0x7f800001, 0xffc00000):
            for index in (0, len(words) - 1):
                changed = words.copy()
                changed[index] = bad
                self.assertFalse(self.backend.all_finite(struct.pack('<6I', *changed)))

    def test_unaligned_bytes_and_invalid_size(self):
        storage = C.create_string_buffer(b'x' + struct.pack('<2f', .25, -.5))
        self.assertEqual(self.backend._finite(C.addressof(storage) + 1, 8), 1)
        self.assertEqual(self.backend._finite(C.addressof(storage) + 1, 7), 0)
        self.assertEqual(self.backend._finite(None, 4), 0)
        self.assertEqual(self.backend._finite(None, 0), 1)

    def test_legacy_fallback(self):
        with patch.object(self.backend, '_finite', None):
            self.assertTrue(self.backend.all_finite(struct.pack('<2f', .25, -.5)))
            self.assertFalse(self.backend.all_finite(struct.pack('<f', float('nan'))))

    def test_native_input_rejection_precedes_compute(self):
        body = struct.pack('<IIIIII', 0, 1, 1, 1, 0, 0) + struct.pack('<f', .25) * W.H
        self.assertEqual(W.task(body, 1, 512, 0, 1, finite_check=self.backend.all_finite)[3], [(0, 0)])
        for bad in (float('nan'), float('inf'), -float('inf')):
            with self.assertRaisesRegex(ValueError, 'non-finite activation'):
                W.task(body[:-4] + struct.pack('<f', bad), 1, 512, 0, 1,
                       finite_check=self.backend.all_finite)

    def fake_compute(self, value):
        def compute(handle, gu, down, layer, x, nt, n, rows, ids, pointers, k, output):
            for i in range(n * W.H):
                output[i] = value
            return 1
        return compute

    def source(self):
        class Source:
            layers = [(18, 20)]
            buffer = C.create_string_buffer(16)
            def pointer(self, layer, expert):
                return self.buffer
        return Source()

    def test_output_snapshot_survives_smaller_next_task(self):
        b, source = self.backend, self.source()
        activation = struct.pack('<f', .25) * (2 * W.H)
        with patch.object(b.lib, 'strata_network_compute', self.fake_compute(2.)):
            first = b.compute(source, 0, 2, 2, [(0, 0), (3, 1)], activation)
        with patch.object(b.lib, 'strata_network_compute', self.fake_compute(3.)):
            second = b.compute(source, 0, 1, 1, [(0, 0)], activation[:W.H * 4])
        self.assertEqual(first, struct.pack('<f', 2.) * (2 * W.H))
        self.assertEqual(second, struct.pack('<f', 3.) * W.H)
        self.assertEqual(set(b.last_timings), {'prepare_ms', 'native_ms', 'validate_ms', 'serialize_ms'})

    def test_non_finite_output_poisoning_native_and_legacy(self):
        b, source = self.backend, self.source()
        for native in (True, False):
            b.failed = False
            finite = b._finite if native else None
            with patch.object(b, '_finite', finite), patch.object(b.lib, 'strata_network_compute', self.fake_compute(float('nan'))):
                with self.assertRaisesRegex(RuntimeError, 'non-finite results'):
                    b.compute(source, 0, 1, 1, [(0, 0)], b'\0' * (W.H * 4))
            with self.assertRaisesRegex(RuntimeError, 'restart the worker'):
                b.compute(source, 0, 1, 1, [(0, 0)], b'\0' * (W.H * 4))

    def test_reusable_buffer_geometry_is_checked(self):
        source = self.source()
        for nt, k, entries, data in ((9, 1, [(0, 0)], b''), (1, 11, [(0, 0)], b''),
                                     (1, 1, [(0, 0)], b''), (1, 1, [(0, 0), (1, 1)], b'\0' * (W.H * 4))):
            with self.assertRaisesRegex(ValueError, 'geometry'):
                self.backend.compute(source, 0, nt, k, entries, data)

    def test_weighted_reduction_routing_order_and_empty_position(self):
        b = self.backend
        self.assertTrue(b.can_reduce)
        # Unsorted rows, including cancellation: sum follows router order.
        rows = (C.c_int32 * 3)(2, 0, 1)
        weights = (C.c_float * 3)(1., 1., 1.)
        parts = (C.c_float * (3 * W.H))(*([3.] * W.H + [1.e8] * W.H + [-1.e8] * W.H))
        out = (C.c_float * (2 * W.H))()
        self.assertEqual(b._reduce(parts, rows, weights, 2, 3, 3, out), 1)
        self.assertEqual(list(out[:W.H]), [3.] * W.H)
        self.assertEqual(list(out[W.H:]), [0.] * W.H)
        rows[2] = 0
        self.assertEqual(b._reduce(parts, rows, weights, 2, 3, 3, out), 0)
        rows[2] = 1
        weights[2] = float('inf')
        self.assertEqual(b._reduce(parts, rows, weights, 2, 3, 3, out), 0)

    def test_weighted_compute_snapshot_and_overflow(self):
        b, source = self.backend, self.source()
        with patch.object(b.lib, 'strata_network_compute', self.fake_compute(2.)):
            got = b.compute(source, 0, 2, 2, [(1, 0), (0, 1)], b'\0' * (2 * W.H * 4), [.25, .75])
        self.assertEqual(got, struct.pack('<f', 2.) * W.H + b'\0' * (W.H * 4))
        with patch.object(b.lib, 'strata_network_compute', self.fake_compute(3.e38)):
            with self.assertRaisesRegex(RuntimeError, 'non-finite network reduction result'):
                b.compute(source, 0, 1, 2, [(0, 0), (1, 1)], b'\0' * (W.H * 4), [2., 2.])
        self.assertTrue(b.failed)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library', type=Path, required=True)
    args = parser.parse_args()
    ValidationTests.library = args.library
    unittest.main(argv=['test_network_validation'], verbosity=2)
