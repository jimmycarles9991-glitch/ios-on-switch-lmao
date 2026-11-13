"""ibprogram's rules refuse the first passes the chip refused on 12 Nov and take the ones it ran."""

import unittest

from tools.benchloop import ibprogram


def load(*words: int, enable: int = 1, pointer: int = 0) -> list[list[int]]:
    return [[0x0DF0, pointer], *[[0x0DF4, w] for w in words], [0x0DD4, len(words)], [0x0DD0, enable]]


class IbprogramTest(unittest.TestCase):
    def test_first_passes_the_chip_refused(self) -> None:
        # hue, sat/col, vcm: MAX of an alpha and constant 1 or 6; PBR, TEMP0, TEMP1 read at pass 0
        for word in (0x3000D659, 0x3000D459, 0x3200D799, 0x0000DE59, 0x0000C029, 0x00009029, 0x00001E01, 0x0000DA01):
            self.assertIsNotNone(ibprogram.refusal(load(word, 0x9029)), hex(word))

    def test_first_passes_the_chip_ran(self) -> None:
        for word in (0x3000D619, 0x00001659, 0x0000D601, 0x0000D001, 0x0000D029, 0x31805029, 0x00091001):
            self.assertIsNone(ibprogram.refusal(load(word, 0x9029)), hex(word))

    def test_pointer_and_alpha_rules(self) -> None:
        self.assertIsNotNone(ibprogram.refusal(load(0x91001, pointer=1)))
        self.assertIsNotNone(ibprogram.refusal(load(0x91001, enable=3)))
        to_alpha = 0x12801029
        self.assertIsNotNone(ibprogram.refusal(load(0x91001, to_alpha, 0x9029, enable=1)))
        self.assertIsNone(ibprogram.refusal(load(0x91001, to_alpha, 0x9029, enable=3)))

    def test_renumbering_moves_the_first_pass_to_constant_zero(self) -> None:
        methods = [[0x1480, 0x34CD], [0x1490, 0x068E], *load(0x3000D659, 0x220FDC01)]
        out = ibprogram.renumbered(methods)
        self.assertEqual(out[:2], [[0x1490, 0x34CD], [0x1480, 0x068E]])
        self.assertEqual([v for m, v in out if m == 0x0DF4], [0x3000D619, 0x220FDC41])
        self.assertIsNone(ibprogram.refusal(out))


if __name__ == '__main__':
    unittest.main()
