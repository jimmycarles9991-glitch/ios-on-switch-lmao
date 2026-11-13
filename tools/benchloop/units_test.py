"""Kernel outputs, compressed targets and 16-bit stores: valid batches, and the fake device's outputs judged right."""

import tempfile
import unittest
from pathlib import Path

from tools.benchloop import protocol, report, rop, session, texels, units
from tools.benchloop.sm53 import latency, ops


class UnitsTest(unittest.TestCase):
    def test_compressed_draws_compare_with_a_plain_target(self) -> None:
        experiments = units.compressed()
        protocol.batch('compressed', experiments[: protocol.MAX_EXPERIMENTS])
        self.assertTrue(all(e['compressed'] and e['compare'] == 'plain' for e in experiments))

    def test_compute_stall_words_are_the_interpreters(self) -> None:
        recipes = [r for r in ops.FIXED if r.key in ('FADD', 'IADD')]
        planned = latency.compute_stall(recipes)
        self.assertEqual(len(planned), 2 * len(latency.STALL_DELAYS))
        with tempfile.TemporaryDirectory() as scratch:
            results = session.fake(Path(scratch) / 'device').run_all(planned, 'cstall', Path(scratch) / 'out')
        found = report.analysis('cstall', planned, results, [])['cstall']
        self.assertTrue(all(row['right'] for row in found['rows']), found['rows'][:2])
        self.assertEqual(found['latency'], {'FADD': 1, 'IADD': 1}, 'the fake GPU never reads a stale register')

    def test_sixteen_bit_stores_cover_their_ties_sparsely(self) -> None:
        experiments = rop.generate16()
        protocol.batch('store16', experiments)
        self.assertEqual({e['target']['format'] for e in experiments}, set(rop.FORMATS16))
        self.assertEqual(texels.encode((0.5, 0.0, 0.0, 1.0), 'r16unorm', 'gm20b').hex(), 'ff7f')


if __name__ == '__main__':
    unittest.main()
