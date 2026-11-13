"""rungcheck.py's stand-ins for the fetch: the chip's 8-bit blender with a form's factors, and an iterated program."""

import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from tools.gpu.maxwell import rungcheck


def per_target(colour: int, op: int, source: int, dest: int) -> dict[int, int]:
    base = 0x1E00 + 0x20 * colour
    return {base + 4: op, base + 8: source, base + 0xC: dest, base + 0x10: op, base + 0x14: source, base + 0x18: dest}


class FixedBlend(unittest.TestCase):
    def test_source_over_rounds_the_integer_sum_once(self):
        blob = per_target(0, 0x8006, 0x4001, 0x4303)
        # S = (0.5, 0.25, 0, 0.5) premultiplied, D = (100, 50, 200, 255): S8 + D (255 - Sa8) / 255, nearest
        got = rungcheck.fixed_blend(blob, 0, [0.5, 0.25, 0.0, 0.5], [100, 50, 200, 255])
        s = [rungcheck.store(x) for x in (0.5, 0.25, 0.0, 0.5)]
        want = [min(255, int((s[c] * 255 + d * (255 - s[3])) / 255 + 0.5)) for c, d in enumerate([100, 50, 200, 255])]
        self.assertEqual(got, want)

    def test_clear_writes_zero(self):
        blob = per_target(0, 0x8006, 0x4000, 0x4000)
        self.assertEqual(rungcheck.fixed_blend(blob, 0, [0.7, 0.2, 0.1, 0.9], [9, 8, 7, 255]), [0, 0, 0, 0])

    def test_another_colour_reads_its_own_block(self):
        blob = per_target(1, 0x8006, 0x4000, 0x4001)
        self.assertEqual(rungcheck.fixed_blend(blob, 1, [1.0, 1.0, 1.0, 1.0], [3, 4, 5, 6]), [3, 4, 5, 6])


class IteratedBlend(unittest.TestCase):
    def test_multiply_matches_the_formula_within_a_code(self):
        source, dest = [0.4, 0.2, 0.1, 0.5], [128, 64, 32, 255]
        d = [x / 255 for x in dest]
        exact = [source[c] * d[c] + source[c] * (1 - d[3]) + d[c] * (1 - source[3]) for c in range(3)]
        exact.append(source[3] + d[3] * (1 - source[3]))
        got = rungcheck.iterated_blend(rungcheck.table.PROGRAMS['mpy'], source, dest)
        for g, e in zip(got, exact, strict=True):
            self.assertLessEqual(abs(g - rungcheck.store(e)), 1)


class Outputs(unittest.TestCase):
    def test_each_written_colour_takes_a_vec4(self):
        image = bytearray(80)
        struct.pack_into('<I', image, rungcheck.OMAP_TARGETS, 0xF01)  # colour 0: R (a packed view), colour 2: RGBA
        self.assertEqual(rungcheck.target_outputs(bytes(image), 0), (0, 1))
        self.assertEqual(rungcheck.target_outputs(bytes(image), 2), (4, 4))
        self.assertEqual(rungcheck.nakstat_outputs(bytes(image)), 8)
        self.assertEqual(rungcheck.written_registers(bytes(image)), [0, 4, 5, 6, 7])


if __name__ == '__main__':
    unittest.main()
