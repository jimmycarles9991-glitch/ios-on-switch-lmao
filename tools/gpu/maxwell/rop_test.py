"""rop.py's store rule against the device bench's readings and an exact reference."""

import math
import random
import struct
import unittest
from fractions import Fraction

from tools.gpu.maxwell import rop


def f32(x: float) -> int:
    return struct.unpack('<I', struct.pack('<f', x))[0]


def reference(bits: int, n: int) -> int:
    v = struct.unpack('<f', struct.pack('<I', bits))[0]
    if math.isnan(v) or v <= 0:
        return 0
    if v >= 1:
        return (1 << n) - 1
    u = int(Fraction(v) * (1 << (n + 4)))
    return (u * ((1 << n) - 1) + (1 << (n + 3)) - 1) >> (n + 4)


class RopTest(unittest.TestCase):
    def test_bench_readings(self):
        # diag4 (20261111-155823), bgra8unorm
        for value, code in ((0.25, 0x40), (0.5, 0x7F), (1.0, 0xFF), (0.75, 0xBF)):
            self.assertEqual(rop.store_code(f32(value), 8), code)
        for k in range(6):
            self.assertEqual(rop.store_code(f32((k + 0.5) / 255), 8), k)
        self.assertEqual(rop.store_code(0x7FC00000, 8), 0)
        self.assertEqual(rop.store_code(0x7F800000, 8), 255)
        self.assertEqual(rop.store_code(f32(-0.0), 10), 0)
        self.assertEqual([rop.store_code(f32(c / 3), rop.ALPHA2) for c in range(4)], [0, 1, 2, 3])

    def test_matches_the_exact_rule(self):
        rng = random.Random(4)
        values = [rng.getrandbits(32) for _ in range(20000)] + [f32(rng.random()) for _ in range(20000)]
        values += list(range(0, 0x800000, 4099))
        for n in (8, 10, 16):
            for bits in values:
                self.assertEqual(rop.store_code(bits, n), reference(bits, n), (hex(bits), n))

    def test_half_store(self):
        for value in (1.0, -2.0, 65504.0, 1.0009765625, 6.103515625e-05, 5.960464477539063e-08, 1e-10, -0.0):
            want = struct.unpack('<H', struct.pack('<e', value))[0] if value != 1e-10 else 0
            self.assertEqual(rop.store_code(f32(value), rop.HALF), want, value)
        self.assertEqual(rop.store_code(f32(1.0009), rop.HALF), 0x3C00)
        self.assertEqual(rop.store_code(f32(1e9), rop.HALF), 0x7BFF)
        self.assertEqual(rop.store_code(0xFF800000, rop.HALF), 0xFC00)
        self.assertEqual(rop.store_code(0xFFC12345, rop.HALF), 0x7E00)
        rng = random.Random(5)
        for _ in range(20000):
            exact = struct.unpack('<f', struct.pack('<f', rng.uniform(-65504, 65504) * rng.choice((1, 1e-3, 1e-6))))[0]
            nearest = struct.unpack('<e', struct.pack('<e', exact))[0]
            got = struct.unpack('<e', struct.pack('<H', rop.store_code(f32(exact), rop.HALF)))[0]
            self.assertLessEqual(abs(got), abs(exact), exact)
            self.assertTrue(got == nearest or abs(nearest) > abs(exact), exact)

    def test_output_widths(self):
        key = 'samples=1 topo=0 ds=0/0 c0=80:15:0:1 c1=115:0:0:1 c2=90:15:0:1  | v x  | f y'
        self.assertEqual(rop.widths(key, 0xF | 0x3 << 4 | 0x9 << 8), [8, 8, 8, 8, rop.HALF, rop.HALF, 10, rop.ALPHA2])


if __name__ == '__main__':
    unittest.main()
