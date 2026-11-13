"""storecheck.py's verdict of a moved load from what moved, and which colours a state's storage form can leave."""

import struct
import unittest

from tools.gpu.maxwell import storecheck

OWN = (1, 0)


def fp32(x: float) -> float:
    return struct.unpack('<f', struct.pack('<f', x))[0]


class Kind(unittest.TestCase):
    def test_a_store_of_the_load_plus_a_term_is_add_though_the_sum_rounds_in_fp32(self):
        load, term = 0.875, 5 * 2.0**-27
        base, moved = fp32(load + term), fp32(load + storecheck.DELTA + term)
        self.assertNotEqual(moved - base, storecheck.DELTA)
        self.assertEqual(storecheck.kind({OWN}, OWN, base, moved, load), 'add')

    def test_a_term_that_scales_the_load_by_a_little_is_not_add(self):
        load = 1.0
        base, moved = fp32(load * 0.999), fp32((load + storecheck.DELTA) * 0.999)
        self.assertEqual(storecheck.kind({OWN}, OWN, base, moved, load), 'own')

    def test_the_load_written_back_is_kept(self):
        self.assertEqual(storecheck.kind({OWN}, OWN, 0.5, 0.5 + storecheck.DELTA, 0.5), 'kept')

    def test_a_product_or_a_mix_is_its_own(self):
        self.assertEqual(storecheck.kind({OWN}, OWN, 0.5, 0.5 + storecheck.DELTA / 2, 0.5), 'own')

    def test_a_load_that_moves_anything_else_is_read_and_one_that_moves_nothing_unused(self):
        self.assertEqual(storecheck.kind({OWN, ('out', 0)}, OWN, 0.5, 0.75, 0.5), 'read')
        self.assertEqual(storecheck.kind(set(), OWN, 0.5, 0.5, 0.5), 'unused')

    def test_a_load_the_lane_never_made_is_not_kept(self):
        self.assertEqual(storecheck.kind({OWN}, OWN, 0.5, 0.75, None), 'add')


class Leaves(unittest.TestCase):
    @staticmethod
    def found(per_colour: dict[int, list[str]]) -> dict:
        return {(c, ch): {k} for c, kinds in per_colour.items() for ch, k in enumerate(kinds)}

    def test_a_colour_of_unused_kept_and_add_channels_leaves(self):
        self.assertEqual(storecheck.leaves(self.found({1: ['kept', 'add', 'unused', 'unused']}), 0b10), [1])

    def test_another_written_colour_in_the_interlock_keeps_the_state_there(self):
        found = self.found({0: ['own', 'own', 'own', 'own'], 1: ['add', 'add', 'unused', 'unused']})
        self.assertEqual(storecheck.leaves(found, 0b11), [])

    def test_a_read_channel_keeps_its_colour(self):
        self.assertEqual(storecheck.leaves(self.found({1: ['kept', 'read', 'unused', 'unused']}), 0b10), [])


if __name__ == '__main__':
    unittest.main()
