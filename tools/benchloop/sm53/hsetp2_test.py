import random
import struct
import unittest

from tools.benchloop import protocol
from tools.benchloop.sm53 import hsetp2, latency, operands, program, timing_sim
from tools.benchloop.sm53.encode import Pred


def field(w: int, at: int, n: int) -> int:
    return w >> at & (1 << n) - 1


class Hsetp2Test(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.experiments = hsetp2.generate()
        cls.forms = [x for x in cls.experiments if x['meta']['family'] == 'hsetp2']

    def test_one_valid_batch(self):
        protocol.batch('hsetp2', self.experiments)
        self.assertEqual(sum(len(x['meta']['forms']) for x in self.forms), len(hsetp2.variants()))

    def test_fields_as_nak_writes_them(self):
        w = hsetp2.hsetp2((2, 7), 1, 4, 'lt and neg_a a=h1h1 b=h0h0 abs_b').word
        self.assertEqual(field(w, 51, 13), 0b0101_1101_0010_0)
        self.assertEqual((field(w, 3, 3), field(w, 0, 3), field(w, 8, 8), field(w, 20, 8)), (2, 7, 1, 4))
        self.assertEqual((field(w, 47, 2), field(w, 28, 2), field(w, 43, 1), field(w, 44, 1)), (3, 2, 1, 0))
        self.assertEqual(
            (field(w, 30, 1), field(w, 31, 1), field(w, 35, 4), field(w, 39, 3), field(w, 45, 2)), (1, 0, 1, 7, 0)
        )
        w = hsetp2.hsetp2((0, 1), 8, 9, 'nan xor ftz h_and', Pred(5, neg=True)).word
        self.assertEqual(
            (field(w, 6, 1), field(w, 49, 1), field(w, 45, 2), field(w, 39, 4), field(w, 35, 4)), (1, 1, 2, 13, 8)
        )

    def test_predicate_chain_reads_wrong_one_cycle_short(self):
        for lat in (13, 14):
            for delay in (lat - 1, lat):
                prog = latency._stall_preds(hsetp2.RECIPE, delay).fragment()
                regs, _ = timing_sim.run(prog.code, prog.data, timing_sim.Table({'HSETP2': lat}))
                self.assertEqual(regs == program.expected_texel(prog.code, prog.data), delay >= lat, (lat, delay))

    def test_program_writes_what_the_interpreter_computes(self):
        rng = random.Random(5)
        pairs = hsetp2.operand_pairs()
        for x in self.forms:
            forms = [tuple(f) for f in x['meta']['forms']]
            code, data = (protocol.unb64(x['program'][k]) for k in ('code', 'data'))
            self.assertLess(operands.highest_register(code), x['program']['gprs'])
            for col in [0, 1, 23, *rng.sample(range(len(pairs)), 6)]:
                got = program.unpack(program.expected_texel(code, data, (col, 0)))
                self.assertEqual(got, hsetp2.expected(forms, *pairs[col]), (x['id'], col))

    def test_analysis_names_the_form_that_differs(self):
        results = {}
        for x in self.forms:
            forms = [tuple(f) for f in x['meta']['forms']]
            words = [program.pack(*hsetp2.expected(forms, a, b)) for a, b in hsetp2.pairs_of(x)]
            if x is self.forms[0]:
                words[3][0] ^= 1 << 3
            results[x['id']] = {
                'status': 'ok',
                'texels': {'data': protocol.b64(b''.join(struct.pack('<4I', *w) for w in words))},
            }
        found = hsetp2.analyse(self.forms, results)
        off = {
            (eid, name): (r['p_off'], r['q_off'])
            for eid, v in found.items()
            for name, r in v['forms'].items()
            if r['p_off'] or r['q_off']
        }
        self.assertEqual(len(off), 1)
        (eid, name), counts = next(iter(off.items()))
        self.assertEqual(eid, self.forms[0]['id'])
        self.assertEqual(counts, (1, 0))
        self.assertTrue(name.startswith(hsetp2.variants()[14][0]), name)


if __name__ == '__main__':
    unittest.main()
