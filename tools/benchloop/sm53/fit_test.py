"""fit.py's stall verdicts when the device and the interpreter disagree, and its line fits."""

import unittest

from tools.benchloop.sm53 import fit, nak, stats
from tools.benchloop.sm53.latency import STALL_DELAYS

INTERPRETED = 'aa' * 16
STALE = 'bb' * 16
HARDWARE = 'cc' * 16


def _rows(texel_at: dict[int, str | None]) -> list[dict]:
    return [
        {
            'family': 'stall',
            'op': 'FADD',
            'nak': 'FAdd',
            'pred': False,
            'delay': d,
            'status': 'ok' if t is not None else 'missing',
            'texel': t,
            'expected': INTERPRETED,
        }
        for d, t in texel_at.items()
    ]


class FitTest(unittest.TestCase):
    def test_latency_is_where_the_right_texel_starts(self):
        got = fit.fixed(_rows({d: INTERPRETED if d >= 7 else STALE for d in STALL_DELAYS}))['FADD']
        self.assertEqual((got['latency'], got['reference']), (7, 'interpreter'))
        self.assertEqual(got['pattern'], '------+++++++++')

    def test_a_hardware_result_unlike_the_interpreter_becomes_the_reference(self):
        got = fit.fixed(_rows({d: HARDWARE if d >= 9 else STALE for d in STALL_DELAYS}))['FADD']
        self.assertEqual((got['latency'], got['reference']), (9, 'delay-15'))

    def test_the_same_stale_texel_everywhere_is_never_right(self):
        got = fit.fixed(_rows(dict.fromkeys(STALL_DELAYS, STALE)))['FADD']
        self.assertEqual((got['latency'], got['reference']), (None, 'never right'))

    def test_never_right_is_unsafe_and_keeps_nak_in_the_table(self):
        table = nak.table(fit.fixed(_rows(dict.fromkeys(STALL_DELAYS, STALE))))
        self.assertEqual(
            [(d['op'], d['measured'], d['kind']) for d in table['disagreements']], [('FADD', '>15', 'unsafe')]
        )
        self.assertIn('    Op::FAdd(_) => (6, 13),\n', table['rust'])

    def test_overlapping_draws_are_flagged(self):
        rows = [
            {
                'family': 'chain',
                'op': 'IMUL',
                'k': k,
                'timed': True,
                'ns_per_draw': 10.0 * k,
                'first_ns': 1e4 + 20.0 * k,
            }
            for k in (64, 128, 256, 512)
        ]
        self.assertTrue(fit.variable(rows, 1.0)['IMUL']['overlap'])
        rows = [r | {'first_ns': 1e4 + 10.0 * r['k']} for r in rows]
        self.assertFalse(fit.variable(rows, 1.0)['IMUL']['overlap'])

    def test_missing_results_are_unknown_not_wrong(self):
        texels = {d: INTERPRETED if d >= 5 else STALE for d in STALL_DELAYS} | {3: None, 10: None}
        got = fit.fixed(_rows(texels))['FADD']
        self.assertEqual(got['pattern'], '--?-+++++?+++++')
        self.assertEqual(got['latency'], 5)

    def test_line_fit(self):
        fit_ = stats.line([1, 2, 4, 8], [11.0, 21.0, 41.0, 81.0])
        self.assertAlmostEqual(fit_.slope, 10.0)
        self.assertAlmostEqual(fit_.intercept, 1.0)
        self.assertAlmostEqual(fit_.rms, 0.0)


if __name__ == '__main__':
    unittest.main()
