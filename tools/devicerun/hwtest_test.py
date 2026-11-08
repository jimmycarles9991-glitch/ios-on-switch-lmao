"""hwtest.py's checks over synthetic app lines, and its test IDs against hw_test_app's sources."""

import re
import unittest
from pathlib import Path

from tools.devicerun import devicerun, hwtest
from tools.devicerun.rules import FAIL, INFO, NO_DATA, PASS

APP = Path(__file__).resolve().parents[2] / 'hw_test_app'
SOURCES = (APP / 'tests', APP / 'gpu/scenes')
PARAMETERIZED = re.compile(
    r'^(decode\.|playback\.|encode\.|audio\.playback_|image\.(' + '|'.join(hwtest.STILLS) + r')$)'
)


def line(message: str, second: int = 0) -> dict:
    return {
        'timestamp': f'2026-11-07 12:00:{second:02d}.000000+0000',
        'bootUUID': 'B00T',
        'processImagePath': '/Applications/SwitchHWTest.app/SwitchHWTest',
        'subsystem': hwtest.SUBSYSTEM,
        'category': 'results',
        'messageType': 'Default',
        'eventMessage': message,
    }


def run(*messages: str) -> dict:
    records = [line(m, i) for i, m in enumerate(messages)]
    results, _ = devicerun.run_checks(records, hwtest.HWTEST)
    return {r.check.name: r for r in results}


class Verdicts(unittest.TestCase):
    def test_a_run_with_a_failure_and_a_skip(self):
        results = run(
            'HWTEST metal.device PASS apple_family=3 ms=12.0',
            'HWTEST metal.compute FAIL wrong=12 ms=40.1 why=every_element_exact',
            'HWTEST audio.headset SKIP ms=45000.0 why=no_route_change',
            'HWTEST decode.h264_1080p PASS frames=60 fps=212.4 ms=900.0',
            'HWTEST suite FAIL pass=2 fail=1 skip=1 ms=60000.0',
        )
        self.assertEqual(results['hwtest.metal.device'].verdict, PASS)
        self.assertEqual(results['hwtest.metal.compute'].verdict, FAIL)
        self.assertIn('every_element_exact', results['hwtest.metal.compute'].observed)
        self.assertEqual(results['hwtest.suite'].verdict, FAIL)
        self.assertEqual(results['hwtest.skipped'].verdict, INFO)
        self.assertIn('audio.headset', results['hwtest.skipped'].observed)
        self.assertIn('212.4', results['hwtest.decode-fps'].observed)

    def test_gpu_paths_and_pacing_are_reported(self):
        results = run(
            'HWTEST gpu.clear.formats PASS dev=raw raw_checks=228 raw_paths=raw/230,m2v:compute:compute/2 ms=9.0',
            'HWTEST pacing.blur_over FAIL link_frames=300 link_missed=0 link_hist=299/0/0/0 late=0 '
            'present_frames=290 present_missed=12 present_hist=280/9/1/0 ms=6000.0 why=the_display_missed',
            'HWTEST suite FAIL pass=1 fail=1 skip=0 ms=1.0',
        )
        self.assertIn('raw/230,m2v:compute:compute/2', results['hwtest.gpu-paths'].observed)
        self.assertIn('280/9/1/0', results['hwtest.pacing'].observed)
        self.assertEqual(results['hwtest.pacing.blur_over'].verdict, FAIL)

    def test_a_clean_run_passes(self):
        results = run('HWTEST metal.device PASS ms=1.0', 'HWTEST suite PASS pass=70 fail=0 skip=0 ms=1.0')
        self.assertEqual(results['hwtest.suite'].verdict, PASS)
        self.assertEqual(results['hwtest.cpu.multi'].verdict, PASS)

    def test_no_finished_run_is_no_data(self):
        results = run('HWTEST metal.device PASS ms=1.0')
        self.assertEqual(results['hwtest.suite'].verdict, NO_DATA)
        self.assertEqual(results['hwtest.metal.device'].verdict, NO_DATA)

    def test_a_fail_without_a_reason_still_counts(self):
        results = run('HWTEST cpu.single FAIL ms=1.0', 'HWTEST suite FAIL pass=0 fail=1 skip=0 ms=1.0')
        self.assertEqual(results['hwtest.cpu.single'].verdict, FAIL)

    def test_all_checks_add_one_predicate_clause(self):
        self.assertEqual(len({devicerun.clause(c) for c in hwtest.HWTEST}), 1)

    def test_an_id_that_prefixes_another_is_its_own_row(self):
        results = run('HWTEST metal.render.color FAIL ms=1.0 why=x', 'HWTEST suite FAIL pass=0 fail=1 skip=0 ms=1.0')
        self.assertEqual(results['hwtest.metal.render.color'].verdict, FAIL)
        self.assertEqual(results['hwtest.metal.render.depth_stencil'].verdict, PASS)
        results = run('HWTEST metal.compute FAILED ms=1.0', 'HWTEST suite PASS pass=1 fail=0 skip=0 ms=1.0')
        self.assertEqual(results['hwtest.metal.compute'].verdict, PASS)


class AppSources(unittest.TestCase):
    def declared(self) -> dict[str, bool]:
        ids = {}
        for source in sorted(path for directory in SOURCES for path in directory.glob('*.swift')):
            for block in re.split(r'\n(?=struct )', source.read_text()):
                for test_id in re.findall(r'(?:let id = |\bid: )"([a-z0-9_.]+)"', block):
                    ids[test_id] = 'let interactive = true' in block
        return ids

    def test_every_literal_id_is_checked_with_its_interactivity(self):
        declared = self.declared()
        self.assertGreater(len(declared), 40)
        for test_id, interactive in declared.items():
            self.assertIn(test_id, hwtest.TESTS)
            self.assertEqual(hwtest.TESTS[test_id], interactive, test_id)

    def test_every_other_id_is_one_of_the_app_tables(self):
        declared = self.declared()
        for test_id in hwtest.TESTS:
            if test_id not in declared:
                self.assertRegex(test_id, PARAMETERIZED)


if __name__ == '__main__':
    unittest.main()
