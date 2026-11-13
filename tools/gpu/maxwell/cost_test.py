"""cost.py's rates and register-bank reads on hand-built instructions and the image-draw fixture."""

import unittest
from pathlib import Path

from tools.gpu.maxwell import cost, isa

FIXTURE = Path(__file__).with_name('testdata') / 'image_draw_fragment.hex'


def ffma_reg(d: int, a: int, b: int, c: int) -> int:
    return 0x5980 << 48 | c << 39 | b << 20 | a << 8 | d


class CostTest(unittest.TestCase):
    def test_rates(self):
        self.assertEqual(cost.issue_cycles('FFMA_reg'), 1.0)
        self.assertEqual(cost.issue_cycles('SHL_imm'), 2.0)
        self.assertEqual(cost.issue_cycles('ISETP_reg'), 2.25)
        self.assertEqual(cost.issue_cycles('F2F_reg'), 4.0)
        self.assertEqual(cost.issue_cycles('IMUL32I'), 130.0)
        self.assertIsNone(cost.issue_cycles('TEX'))

    def test_banks(self):
        w = ffma_reg(1, 0, 4, 8)
        self.assertEqual(isa.name(w), 'FFMA_reg')
        self.assertEqual(cost.gpr_sources(w, 'FFMA_reg'), [0, 4, 8])
        self.assertEqual(cost.bank_extra([0, 4, 8], distinct=True), 2)
        self.assertEqual(cost.bank_extra([0, 1, 2], distinct=True), 0)
        self.assertEqual(cost.bank_extra([4, 4, 5], distinct=True), 0)
        self.assertEqual(cost.bank_extra([4, 4, 5], distinct=False), 1)
        self.assertEqual(cost.gpr_sources(ffma_reg(1, 255, 4, 255), 'FFMA_reg'), [4])

    def test_program(self):
        p = cost.program(ffma_reg(1, 0, 4, 8).to_bytes(8, 'little').rjust(16, b'\0'))
        self.assertEqual((p['cycles'], p['bank'], p['stalls']), (3.0, 2, 0))
        bundle = (6 << 21 | 1 | 2 << 42).to_bytes(8, 'little') + (1).to_bytes(8, 'little') * 3
        self.assertEqual(cost.stalls(bundle), 6 + 1 + 2)
        fixture = cost.program(isa.from_hex_dump(FIXTURE.read_text()))
        self.assertGreaterEqual(fixture['cycles'], fixture['cycles_overlap'])
        self.assertGreaterEqual(fixture['cycles_overlap'], 63)


if __name__ == '__main__':
    unittest.main()
