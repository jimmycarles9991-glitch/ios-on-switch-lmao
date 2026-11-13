"""The whole loop on a made-up GPU: generated experiments, timing_sim as the device, fit recovering its table."""

import random
import unittest

from tools.benchloop import protocol
from tools.benchloop.sm53 import fit, latency, program, timing_sim

CLOCK_HZ = 921.6e6
OVERHEAD_NS = 14_000.0
NOISE = 0.002
TABLE = timing_sim.Table(
    latency={
        'FADD': 5,
        'IADD': 4,
        'HFMA2': 9,
        'FSETP': 11,
        'LOP32I': 3,
        'MOV': 4,
        'MUFU.RCP': 21,
        'IMUL': 17,
        'I2F': 13,
    },
    issue={'MUFU': 4, 'IMUL': 2, 'HFMA2': 2},
    dual=frozenset({('FFMA', 'IADD')}),
    bank_penalty=1,
)
OPS = ('FADD', 'IADD', 'HFMA2', 'FSETP', 'LOP32I', 'MUFU.RCP', 'IMUL', 'I2F')
PAIRS = (('FFMA', 'IADD'), ('FFMA', 'FFMA'), ('FFMA', 'MUFU'))


def device(e: dict) -> dict:
    """Give the result the fake device reports for one experiment: timing_sim's warp, noisy command buffers."""
    code, data = protocol.unb64(e['program']['code']), protocol.unb64(e['program']['data'])
    regs, cycles = timing_sim.run(code, data, TABLE)
    x0, y0, x1, y1 = e['rect']
    pixels = (x1 - x0) * (y1 - y0)
    per_draw = timing_sim.draw_ns(cycles, pixels, CLOCK_HZ, timing_sim.issue_cycles(code, TABLE))
    rng = random.Random(e['id'])

    def buffer(draws: int) -> float:
        return OVERHEAD_NS * (1 + rng.gauss(0, NOISE)) + draws * per_draw * (1 + rng.gauss(0, NOISE))

    runs = [{'draws': n, 'gpu_ns': [buffer(n) for _ in range(e['repeats'])], 'wall_ns': []} for n in e['draws']]
    area = e['target']['width'] * e['target']['height']
    distinct = [
        {'value': program.texel_hex(regs), 'count': pixels, 'first': [x0, y0]},
        {'value': program.texel_hex([0, 0, 0, 0]), 'count': area - pixels, 'first': [x1 % e['target']['width'], 0]},
    ]
    return {'status': 'ok', 'runs': runs, 'texels': {'distinct': [d for d in distinct if d['count']], 'crc32': ''}}


class LoopTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.experiments = latency.generate(ops=OPS, pairs=PAIRS)
        cls.analysis = fit.analyse(cls.experiments, {e['id']: device(e) for e in cls.experiments})

    def test_clock(self):
        self.assertAlmostEqual(self.analysis['clock']['clock_hz'] / CLOCK_HZ, 1.0, delta=0.005)

    def test_fixed_latencies(self):
        for op in ('FADD', 'IADD', 'HFMA2', 'FSETP', 'LOP32I'):
            got = self.analysis['fixed'][op]
            self.assertEqual(got['latency'], TABLE.latency[op], (op, got))
            self.assertEqual(got['reference'], 'interpreter', op)

    def test_variable_latencies(self):
        for op in ('MUFU.RCP', 'IMUL', 'I2F'):
            self.assertAlmostEqual(self.analysis['variable'][op]['cycles'], TABLE.latency[op], delta=0.3, msg=op)

    def test_issue_cycles(self):
        tput = self.analysis['throughput']
        for op in OPS:
            self.assertAlmostEqual(tput[op]['cycles'], TABLE.issue_of(op), delta=0.05, msg=op)

    def test_dual_issue(self):
        verdicts = {pair: v['dual'] for pair, v in self.analysis['dual'].items()}
        self.assertEqual(verdicts, {'FFMA+IADD': True, 'FFMA+FFMA': False, 'FFMA+MUFU': False})

    def test_reuse(self):
        r = self.analysis['reuse']
        self.assertAlmostEqual(r['bank_penalty'], TABLE.bank_penalty, delta=0.05)
        self.assertAlmostEqual(r['reuse_saves'], 2 * TABLE.bank_penalty, delta=0.05)

    def test_every_exact_experiment_passes_its_pixel_check(self):
        stale = {'stall'}
        wrong = [r['id'] for r in self.analysis['rows'] if r['family'] not in stale and not r['pixels_ok']]
        self.assertEqual(wrong, [])

    def test_nak_table(self):
        table = self.analysis['nak']
        self.assertEqual(table['json']['FAdd']['gpr'], 5)
        self.assertEqual(table['json']['FSetP']['pred'], 11)
        self.assertFalse(table['json']['FAdd']['measured']['pred'])
        for arm in ('Op::FAdd(_) => (5, 13),', 'Op::FSetP(_) => (6, 11),', 'Op::HFma2(_) => (9, 14),'):
            self.assertIn(f'    {arm}\n', table['rust'])
        self.assertIn('    | Op::HMul2(_)\n', table['rust'])
        self.assertIn('    | Op::HSetP2(_) => (13, 14),\n', table['rust'])
        unsafe = {d['op'] for d in table['disagreements'] if d['kind'] == 'unsafe'}
        self.assertEqual(unsafe, set())
        slack = {d['op']: d['measured'] for d in table['disagreements'] if d['kind'] == 'slack'}
        self.assertEqual(slack, {'FADD': 5, 'IADD': 4, 'HFMA2': 9, 'FSETP': 11, 'LOP32I': 3})

    def test_one_warp_fits_agree_with_single_draws(self):
        c = self.analysis['clock']
        self.assertAlmostEqual(c['ns_per_cycle_first'] / c['ns_per_cycle'], 1.0, delta=0.02)
        flagged = [op for op, v in self.analysis['variable'].items() if v['overlap']]
        flagged += [f'{op}.d{d}' for op, v in self.analysis['dual'].items() for d in (0, 1) if v[f'd{d}']['overlap']]
        self.assertEqual((c['overlap'], flagged), (False, []))

    def test_tsv_has_a_row_per_experiment(self):
        self.assertEqual(len(fit.tsv(self.analysis).splitlines()), len(self.experiments) + 1)


if __name__ == '__main__':
    unittest.main()
