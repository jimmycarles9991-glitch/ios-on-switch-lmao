"""tools/devicerun over synthetic `log show --style ndjson` lines and crash reports."""

import contextlib
import io
import re
import unittest
from pathlib import Path

from tools.devicerun import checks, devicerun, rules
from tools.devicerun.rules import FAIL, INFO, NO_DATA, PASS, Check

TEST = Path(__file__).resolve().parent / 'test'
NDJSON = TEST / 'device.ndjson'


def run(*check_list: Check) -> dict[str, rules.Result]:
    results, _ = devicerun.run_checks(devicerun.read_ndjson(str(NDJSON)), list(check_list))
    return {r.check.name: r for r in results}


class Checks(unittest.TestCase):
    def test_every_check_is_well_formed(self):
        names = [c.name for group in checks.GROUPS.values() for c in group]
        self.assertEqual(len(names), len(set(names)))
        for group in checks.GROUPS.values():
            for c in group:
                self.assertTrue(c.rule in {rules.REPORT, rules.TIMELINE} or rules.RULE.match(c.rule), c.name)
                self.assertTrue(all(c.literals()), c.name)
                if c.regex:
                    re.compile(c.regex)
                self.assertTrue(not c.needs or c.needs in names, c.name)
                self.assertTrue(not c.against or c.against in names, c.name)

    def test_predicate_escapes_and_covers_every_check(self):
        check = Check('q', 'o', 'say "hi" \\ there', process='kernel', subsystem='a.b', level='error')
        self.assertEqual(
            devicerun.clause(check),
            '((eventMessage CONTAINS "say \\"hi\\" \\\\ there") AND process BEGINSWITH "kernel" '
            'AND subsystem CONTAINS "a.b" AND messageType == error)',
        )
        self.assertIn('BEGINSWITH "SwitchFrame"', devicerun.predicate([check]))


class Rules(unittest.TestCase):
    def test_counts_pass_and_fail(self):
        got = run(
            Check('lskdd', 'o', 'Successfully spawned lskdd[', process='launchd'),
            Check('absent', 'o', 'never logged'),
            Check('present', 'o', 'TegraNVJPG: ', 'count > 0', process='kernel'),
        )
        self.assertEqual(got['lskdd'].verdict, FAIL)
        self.assertEqual(got['absent'].verdict, PASS)
        self.assertEqual(got['present'].verdict, PASS)

    def test_needs_turns_absence_into_no_data(self):
        got = run(
            Check('lane', 'o', 'SwitchUSB: SuperSpeed lane up', 'count > 0'),
            Check('reset', 'o', 'RESET_SSPI not taken', needs='lane'),
        )
        self.assertEqual(got['lane'].verdict, FAIL)
        self.assertEqual(got['reset'].verdict, NO_DATA)

    def test_boots_and_rate(self):
        got = run(
            Check('mg', 'o', 'MobileGestalt cache dropped', 'boots <= 1', show=('boot',)),
            Check('maild', 'o', 'Successfully spawned', 'rate <= 1', subsystem='com.apple.email.maild'),
        )
        self.assertEqual(got['mg'].verdict, FAIL)
        self.assertIn('2 boots', got['mg'].observed)
        self.assertEqual(got['maild'].verdict, PASS)

    def test_field_metrics_and_where(self):
        got = run(
            Check('nvjpg', 'o', 'TegraNVJPG: ', 'sum(decodes) > 0', regex=r'decodes=(?P<decodes>\d+)'),
            Check('imports', 'o', 'tegra-gm20b: imports ', 'p99(p99) <= 4000', regex=checks.GROUPS['gpu'][0].regex),
            Check(
                'idle',
                'o',
                'SwitchDisplay stats: ',
                'max(irqs/ms*1000) < 5',
                regex=r'(?P<ms>\d+) ms: (?P<irqs>\d+) DC A interrupts, (?P<swaps>\d+) swaps',
                where='swaps == 0',
            ),
            Check('none', 'o', 'TegraNVJPG: ', 'sum(missing) > 0', regex=r'decodes=(?P<decodes>\d+)'),
        )
        self.assertEqual(got['nvjpg'].verdict, PASS)
        self.assertEqual(got['imports'].verdict, FAIL)
        self.assertIn('5200', got['imports'].observed)
        self.assertEqual(got['idle'].verdict, PASS)
        self.assertIn('0.30', got['idle'].observed)
        self.assertEqual(got['none'].verdict, NO_DATA)

    def test_against_and_grouping(self):
        by_name = {c.name: c for c in checks.GROUPS['gpu'] + checks.GROUPS['refusals']}
        got = run(by_name['gpu.surface-locks'], by_name['gpu.rainbow'], by_name['refusal.user'])
        self.assertEqual(got['gpu.rainbow'].verdict, INFO)
        self.assertIn('2 surfaces, 1 without a gpu.surface-locks line: 78', got['gpu.rainbow'].observed)
        self.assertIn('backboardd/raw/format', got['refusal.user'].observed)

    def test_kernel_refusal_line(self):
        refusal = next(c for c in checks.GROUPS['refusals'] if c.name == 'refusal.kernel')
        self.assertIn('kernel_task/SwitchVIC/pitch x1', run(refusal)['refusal.kernel'].observed)


class Reports(unittest.TestCase):
    def test_frames_and_crashes(self):
        _, frame_lines = devicerun.run_checks(devicerun.read_ndjson(str(NDJSON)), [])
        frame_rows = {r.check.name: r for r in devicerun.frame_rows(frame_lines)}
        self.assertEqual(frame_rows['frames.interaction'].verdict, FAIL)
        self.assertIn('p50 1 p99 3 periods', frame_rows['frames.interaction'].observed)
        crash = {r.check.name: r for r in devicerun.crash_rows(str(TEST / 'crashes'))}
        self.assertEqual(crash['crash.crashes'].verdict, FAIL)
        self.assertIn('SpringBoard x1', crash['crash.crashes'].observed)
        self.assertEqual(crash['crash.panics'].verdict, PASS)
        self.assertIn('JetsamEvent x1', crash['crash.jetsam'].observed)
        self.assertEqual(devicerun.crash_rows(None)[0].verdict, NO_DATA)

    def test_boot_label_selects_checks_and_steps(self):
        names = {c.name for group in devicerun.selected('A').values() for c in group}
        self.assertIn('gpu.rf16-rounding', names)
        self.assertNotIn('gpu.tiled-cache-on', names)
        self.assertIn('gpu.tiled-cache-on', {c.name for group in devicerun.selected(None).values() for c in group})
        self.assertIn('rf16-rounding', devicerun.checklist('A'))
        self.assertNotIn('rf16-rounding', devicerun.checklist('B'))

    def test_whole_run_exit_code(self):
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(devicerun.main([str(NDJSON), '--ndjson']), 1)
        self.assertIn('launchd.lskdd', out.getvalue())


if __name__ == '__main__':
    unittest.main()
