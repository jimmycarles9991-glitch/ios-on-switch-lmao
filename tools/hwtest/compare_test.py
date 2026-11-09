"""tools/hwtest/compare.py over synthetic runs of one app on one build."""

import unittest
from pathlib import Path

from tools.hwtest import compare
from tools.hwtest.reports import Report

BOOT = '6A25F6C4-A931-4851-9437-EDE961F57EC1'


def entry(test: str, verdict: str, why: str = '', subsystem: str = 'Metal / GPU', **values: object) -> dict:
    fields = [f'{k}={v}' for k, v in values.items()]
    log = ' '.join(['HWTEST', test, verdict, *fields, 'ms=1.0'] + ([f'why={why.replace(" ", "_")}'] if why else []))
    return {'id': test, 'verdict': verdict, 'detail': why, 'subsystem': subsystem, 'metrics': values, 'log': log}


def report(minute: int, *entries: dict, status: str = 'done', current: str | None = None, **identity: str) -> Report:
    data = {
        'schema': 2,
        'run': {
            'id': f'{BOOT}-{minute}',
            'boot_uuid': BOOT,
            'started': f'2026-11-08T22:{minute:02d}:00.000Z',
            'status': status,
            'current': current,
            'order': [e['id'] for e in entries] + ([current] if current else []),
        },
        'identity': {'bundle_id': 'com.brunostjohn.switch.hwtest', 'build': '24B119', **identity},
        'summary': {'pass': sum(e['verdict'] == 'PASS' for e in entries), 'planned': len(entries)},
        'results': list(entries),
    }
    return Report(Path(f'{minute}.json'), data)


class Verdicts(unittest.TestCase):
    def test_regressions_fixes_and_new_failures_are_told_apart(self):
        earlier = [
            report(1, entry('a', 'PASS'), entry('b', 'FAIL', 'old'), entry('c', 'PASS'), entry('d', 'PASS')),
            report(2, entry('a', 'PASS'), entry('b', 'FAIL', 'old'), entry('c', 'SKIP'), entry('d', 'FAIL', 'x')),
        ]
        latest = report(3, entry('a', 'FAIL', 'wrong pixels', wrong=4), entry('b', 'PASS'), entry('c', 'FAIL', 'n'))
        latest.data['results'].append(entry('d', 'FAIL', 'x'))
        changes = compare.compare(latest, earlier)
        self.assertEqual(changes.regressions, ['a: wrong pixels  [wrong=4 ms=1.0]'])
        self.assertEqual(changes.fixes, ['b'])
        self.assertEqual(changes.new_failures, ['c: n  [ms=1.0]'])
        self.assertEqual(len(changes.still_failing), 1)
        self.assertTrue(changes.still_failing[0].startswith('d (passed 2026-11-08 22:01:00'))

    def test_a_planned_test_missing_from_a_finished_run_is_named(self):
        previous = report(1, entry('a', 'PASS'), entry('b', 'PASS'))
        latest = report(2, entry('a', 'PASS'))
        latest.data['run']['order'].append('b')
        self.assertEqual(compare.compare(latest, [previous]).missing, ['b'])


class Numbers(unittest.TestCase):
    def test_moves_beyond_the_band_count_both_ways(self):
        previous = report(1, entry('g', 'PASS', gpu_ms=10.0, gflops=80.0, wrong=0))
        latest = report(2, entry('g', 'PASS', gpu_ms=12.5, gflops=81.0, wrong=3))
        changes = compare.compare(latest, [previous])
        self.assertEqual(changes.slower, ['g gpu_ms: 10 -> 12.5 (+25%)'])
        self.assertEqual(changes.faster, [])

    def test_below_the_best_earlier_run_is_flagged_even_when_last_run_matched(self):
        runs = [report(1, entry('g', 'PASS', fps=60.0)), report(2, entry('g', 'PASS', fps=40.0))]
        latest = report(3, entry('g', 'PASS', fps=41.0))
        changes = compare.compare(latest, runs)
        self.assertEqual(changes.slower, [])
        self.assertEqual(changes.below_best, ['g fps: 41, best 60'])

    def test_sm53_runs_against_plain_runs_are_not_compared(self):
        previous = report(1, entry('g', 'PASS', gpu_ms=10.0), nak_debug='')
        latest = report(2, entry('g', 'FAIL', 'skew', gpu_ms=30.0), nak_debug='sm53_probe')
        changes = compare.compare(latest, [previous])
        self.assertEqual((changes.slower, changes.regressions), ([], []))
        self.assertEqual(changes.not_comparable, ['g: nak_debug changed (- -> sm53_probe) (PASS -> FAIL)'])


class Summary(unittest.TestCase):
    def test_an_unfinished_run_says_where_and_why_it_stopped(self):
        latest = report(2, entry('a', 'PASS'), status='running', current='gpu.ca.sweep')
        latest.mac = {
            'health': {'panics': ['panic-2026-11-08-221030.log'], 'crashes': {'backboardd': 1}, 'boot_s': 41.5}
        }
        text = compare.summary(latest, [])
        self.assertIn('boot to SpringBoard 41.5 s', text)
        self.assertIn('STOPPED at gpu.ca.sweep', text)
        self.assertIn('kernel panic saved: panic-2026-11-08-221030.log', text)
        self.assertIn('crashes backboardd 1', text)
        self.assertIn('no earlier run of this app on this build', text)

    def test_a_run_the_person_closed_is_interrupted_not_stopped(self):
        latest = report(2, entry('a', 'PASS'), status='running', current='gpu.blur.coreimage')
        latest.data['run']['interruption'] = 'closed'
        text = compare.summary(latest, [])
        self.assertIn('INTERRUPTED at gpu.blur.coreimage: the app was closed, not a crash', text)
        self.assertNotIn('STOPPED', text)

    def test_long_sections_are_cut(self):
        previous = report(1, *[entry(f't{i}', 'PASS') for i in range(15)])
        latest = report(2, *[entry(f't{i}', 'FAIL', 'bad') for i in range(15)])
        text = compare.summary(latest, [previous])
        self.assertIn('REGRESSIONS (passed last run) (15)', text)
        self.assertIn('+5 more', text)


if __name__ == '__main__':
    unittest.main()
