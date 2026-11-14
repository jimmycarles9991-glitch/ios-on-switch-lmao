"""The rounding experiments tell the models apart, pick the fake device's own (nearest), and the GM20B model holds."""

import struct
import tempfile
import unittest
from fractions import Fraction
from pathlib import Path

from tools.benchloop import protocol, report, rop, session, texels


def metal(v: float, bits: int) -> int:
    top = (1 << bits) - 1
    return 0 if not v > 0 else top if v >= 1 else int(Fraction(v) * top + Fraction(1, 2))


class RopTest(unittest.TestCase):
    def test_halves_store_as_the_chip_does(self) -> None:
        # bench loop rop1: toward zero, a finite value past the range as 65504, infinities kept, NaN 0x7e00
        stored = texels.encode((65520.0, -1e30, float('nan'), float('-inf')), 'rgba16float')
        self.assertEqual(struct.unpack('<4H', stored), (0x7BFF, 0xFBFF, 0x7E00, 0xFC00))
        below = struct.unpack('<f', struct.pack('<I', 0x3F801FFF))[0]
        stored = texels.encode((below, 2.0**-25, -0.0, 65504.0), 'rgba16float')
        self.assertEqual(struct.unpack('<4H', stored), (0x3C00, 0x0000, 0x8000, 0x7BFF))

    def test_every_unorm_format_encodes_and_decodes(self) -> None:
        colour = (0.0, 0.25, 0.5, 1.0)
        for fmt, bits in texels.UNORM_BITS.items():
            stored = texels.encode(colour, fmt)
            self.assertEqual(len(stored), protocol.FORMATS[fmt][1], fmt)
            want = [texels.unorm(c, n) for c, n in zip(colour, bits, strict=False)]
            self.assertEqual(list(texels.codes(stored, fmt)), want, fmt)

    def test_models_disagree_on_the_values_sent(self) -> None:
        vs = rop.values(8)
        for a in texels.ROUNDING:
            for b in texels.ROUNDING:
                if a != b:
                    self.assertTrue(any(texels.unorm(v, 8, a) != texels.unorm(v, 8, b) for v in vs), (a, b))

    def test_experiments_are_valid(self) -> None:
        for e in rop.generate():
            protocol.validate_experiment(e)

    def test_fake_device_rounds_to_nearest(self) -> None:
        planned = rop.generate(('bgra8unorm', 'rgb10a2unorm'), ('noblend',))
        with tempfile.TemporaryDirectory() as scratch:
            results = session.fake(Path(scratch) / 'device').run_all(planned, 'rop', Path(scratch) / 'out')
        verdicts: dict[str, set[str]] = {}
        for row in report.analysis('rop', planned, results, [])['rop']:
            fmt = row['id'].split('-')[1]
            verdicts[fmt] = verdicts.get(fmt, set(row['verdict'])) & set(row['verdict'])
        self.assertEqual(verdicts, {'bgra8unorm': {'nearest'}, 'rgb10a2unorm': {'nearest'}})

    def test_gm20b_model_reproduces_probe_v1(self) -> None:
        """Probe v1 stored every half blended: 321, 385 and 257 off Metal's rule, first at 1805/1002/81."""
        halves = [struct.unpack('<e', struct.pack('<H', h))[0] for h in range(0x10000)]
        for bits, count, first in ((8, 321, 0x1805), (10, 385, 0x1002), (16, 257, 0x81)):
            off = [h for h, v in enumerate(halves) if texels.unorm(v, bits, 'gm20b') != metal(v, bits)]
            self.assertEqual((len(off), off[0]), (count, first), bits)

    def test_gm20b_rounds_one_half_down(self) -> None:
        self.assertEqual([texels.unorm(v, 8, 'gm20b') for v in (0.25, 0.5, 0.75, 1.0)], [64, 127, 191, 255])
        self.assertEqual(texels.unorm(0.8333333, 2, 'gm20b'), 3)


if __name__ == '__main__':
    unittest.main()
