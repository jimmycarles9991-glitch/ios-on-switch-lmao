"""The generated experiments: valid batches, and stall chains that a single stale read gets wrong."""

import unittest
from collections import Counter

from tools.benchloop import protocol
from tools.benchloop.sm53 import latency, operands, ops, program, timing_sim


class LatencyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.experiments = latency.generate()

    def test_every_family_fits_in_valid_batches(self):
        families = Counter(x['meta']['family'] for x in self.experiments)
        self.assertEqual(set(families), set(latency.FAMILIES))
        self.assertEqual(self.experiments[0]['meta']['family'], 'ladder')
        chunks = latency.batches(self.experiments)
        self.assertEqual(sum(len(c) for c in chunks), len(self.experiments))
        for i, chunk in enumerate(chunks):
            protocol.batch(f'sm53-{i}', chunk)

    def test_ops_and_families_narrow_the_set(self):
        some = latency.generate(['stall', 'chain'], ['FADD', 'MUFU.RCP'])
        self.assertEqual({x['meta']['op'] for x in some}, {'FADD', 'MUFU.RCP'})
        with self.assertRaises(ValueError):
            latency.generate(['nonsense'])

    def test_only_mufu_and_rro_are_inexact(self):
        inexact = {x['meta']['op'] for x in self.experiments if not x['expect']['exact']}
        self.assertEqual(inexact, {r.key for r in ops.VARIABLE_OPS if not r.exact} | {'FFMA+MUFU'})

    def test_every_stall_chain_reads_wrong_one_delay_short_of_the_latency(self):
        for r in ops.FIXED:
            build = latency._stall_preds if r.pred else latency._stall_gprs
            for lat in (2, 6, 13, 15):
                table = timing_sim.Table({r.key: lat})
                for delay in (lat - 1, lat):
                    prog = build(r, delay).fragment()
                    regs, _ = timing_sim.run(prog.code, prog.data, table)
                    right = regs == program.expected_texel(prog.code, prog.data)
                    self.assertEqual(right, delay >= lat, (r.key, lat, delay))

    def test_variable_chains_wait_on_the_previous_link(self):
        prog = latency.chain([ops.RECIPES['IMUL']])[0]['program']
        links = [d for d in operands.decode(protocol.unb64(prog['code'])) if d.base == 'IMUL']
        self.assertEqual([(d.control.wr_bar, d.control.wait) for d in links[:3]], [(0, 2), (1, 1), (0, 2)])


if __name__ == '__main__':
    unittest.main()
