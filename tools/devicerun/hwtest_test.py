"""hwtest.py's checks over synthetic app lines, and its test IDs against apps/hwtest's sources."""

import plistlib
import re
import unittest
from pathlib import Path

from tools.devicerun import devicerun, hwtest, reports
from tools.devicerun.rules import FAIL, INFO, NA, NO_DATA, PASS

APP = Path(__file__).resolve().parents[2] / 'apps/hwtest'
SHARED = (APP / 'tests', APP / 'gpu/scenes')
BY_IOS = {'17': (APP / 'app17',), '27': (APP / 'app27', APP / 'gpu/scenes27')}
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
            (
                'HWTEST gpu.clear.formats PASS dev=gm20b gm20b_checks=228 '
                'gm20b_paths=gm20b/230,error:compute.nokernel/2 '
                'ms=9.0'
            ),
            'HWTEST pacing.blur_over FAIL link_frames=300 link_missed=0 link_hist=299/0/0/0 late=0 '
            'present_frames=290 present_missed=12 present_hist=280/9/1/0 ms=6000.0 why=the_display_missed',
            'HWTEST suite FAIL pass=1 fail=1 skip=0 ms=1.0',
        )
        self.assertIn('gm20b/230,error:compute.nokernel/2', results['hwtest.gpu-paths'].observed)
        self.assertIn('280/9/1/0', results['hwtest.pacing'].observed)
        self.assertEqual(results['hwtest.pacing.blur_over'].verdict, FAIL)

    def test_a_version_only_test_is_judged_only_in_that_versions_log(self):
        line17 = 'HWTEST suite PASS pass=1 fail=0 skip=0 ios=17 ms=1.0'
        line27 = 'HWTEST suite FAIL pass=0 fail=1 skip=0 ios=27 ms=1.0'
        results = run('HWTEST metal.device PASS ms=1.0', line17)
        self.assertEqual(results['hwtest.ui.glass'].verdict, NA)
        self.assertEqual(results['hwtest.metal.device'].verdict, PASS)
        results = run('HWTEST ui.glass FAIL ms=1.0 why=streaks', line27)
        self.assertEqual(results['hwtest.ui.glass'].verdict, FAIL)
        self.assertEqual(run(line27)['hwtest.pacing.glass_over'].verdict, PASS)
        self.assertEqual(run('HWTEST ui.glass PASS ms=1.0')['hwtest.ui.glass'].verdict, NO_DATA)

    def test_a_summary_without_ios_leaves_version_tests_na(self):
        results = run('HWTEST suite PASS pass=1 fail=0 skip=0 ms=1.0')
        self.assertEqual(results['hwtest.metal4.device'].verdict, NA)
        self.assertEqual(results['hwtest.suite'].verdict, PASS)

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


class MissingKeys(unittest.TestCase):
    def test_a_debug_skip_names_its_key_and_fails_a_release_skip_passes(self):
        got = run(
            'HWTEST gpu.recover.kill_one SKIP ms=1.0 why=missing_key_reopen_us_in_a_debug_runtime',
            'HWTEST gpu.path.state_segments SKIP ms=1.0 why=no_state_segment_binds:_a_release_runtime',
        )['hwtest.missing-key']
        self.assertEqual(got.verdict, FAIL)
        self.assertIn('gpu.recover.kill_one/reopen_us', got.observed)
        self.assertNotIn('state_segments', got.observed)
        self.assertEqual(
            run('HWTEST gpu.path.state_segments SKIP ms=1.0 why=a_release_runtime')['hwtest.missing-key'].verdict, PASS
        )


class OpenCases(unittest.TestCase):
    def test_every_open_id_is_a_test_whose_row_names_its_gap(self):
        ids = set(hwtest.TESTS) | {test_id for tests in hwtest.BY_IOS.values() for test_id in tests}
        self.assertLessEqual(set(hwtest.OPEN), ids)
        notes = {check.name: check.note for check in hwtest.HWTEST}
        for test_id, gap in hwtest.OPEN.items():
            self.assertIn(f'open: {gap}', notes[f'hwtest.{test_id}'])
        self.assertNotIn('open:', notes['hwtest.gpu.gm20b.planes_blits'])


class RollUp(unittest.TestCase):
    def rows(self, *messages: str) -> dict:
        records = [line(m, i) for i, m in enumerate(messages)]
        results, _ = devicerun.run_checks(records, hwtest.HWTEST)
        return {r.check.name: r for r in reports.rolled_up(results)}

    def test_reds_name_the_first_and_count_tests_per_predicate(self):
        rows = self.rows(
            'HWTEST gpu.gm20b.a PASS failed_cbs=1 failed_why=compute/1 ms=1.0',
            'HWTEST gpu.gm20b.b FAIL failed_cbs=1 own_failed=1 own_why=enc.pass/1 ms=1.0 why=x_own_why=none/9',
            'HWTEST gpu.ca.c FAIL failed_cbs=3 foreign_failed=3 foreign_why=none/2,enc.pass/1 ms=1.0 why=foreign',
            'HWTEST gpu.ca.c FAIL foreign_failed=1 foreign_why=none/1 ms=1.0 why=foreign',
            'HWTEST gpu.gm20b.d FAIL wrong=3 ms=1.0 why=pixels',
        )
        self.assertNotIn(hwtest.REDS, rows)
        self.assertEqual(rows['hwtest.first-red'].verdict, INFO)
        self.assertEqual(rows['hwtest.first-red'].observed, 'boot B00T: gpu.gm20b.b (own enc.pass/1)')
        self.assertEqual(rows['hwtest.red.enc.pass'].observed, '2 tests reddened, first gpu.gm20b.b (own)')
        self.assertEqual(rows['hwtest.red.none'].observed, '1 test reddened, first gpu.ca.c (foreign)')
        self.assertNotIn('hwtest.red.compute', rows)
        headline = reports.headline([('hwtest', list(rows.values()))])
        self.assertIn('first-red: boot B00T: gpu.gm20b.b (own enc.pass/1); red.enc.pass: 2 tests', headline)

    def test_skips_count_every_test_that_ran_one(self):
        rows = self.rows(
            'HWTEST gpu.gm20b.invalid_requests PASS skipped_calls=5 skipped_why=call.blit/5 ms=1.0',
            'HWTEST gpu.ca.blur_chain FAIL skipped_calls=1 skipped_why=call.blit/1 ms=1.0 why=raw:_skipped',
            'HWTEST gpu.gm20b.e PASS samples_skipped_why=call.icb/1 ms=1.0',
        )
        self.assertEqual(
            rows['hwtest.skipped-calls.call.blit'].observed, 'boot B00T: 2 tests, first gpu.gm20b.invalid_requests'
        )
        self.assertNotIn('hwtest.skipped-calls.call.icb', rows)
        self.assertEqual(rows['hwtest.first-red'].verdict, NO_DATA)
        self.assertEqual(self.rows('HWTEST gpu.gm20b.a PASS ms=1.0')['hwtest.skipped-calls'].verdict, NO_DATA)

    def test_a_kill_test_window_runs_back_from_its_line_by_its_duration(self):
        results = run(
            'HWTEST gpu.recover.kernel_second_timeout FAIL state=4 killed=2 kill_ms=3.0 ms=2500.5 why=ms=7',
            'HWTEST gpu.recover.kernel_second_timeout SKIP state=1 killed=0 ms=900.0 why=direct_scanout',
            'HWTEST gpu.recover.kill_one PASS killed=1 ms=900.0',
        )
        self.assertEqual([f['ms'] for f in results[hwtest.KILL_WINDOWS].matches], ['2500.5'])

    def test_a_clean_boot_after_a_red_one_rolls_up_clean(self):
        older = line('HWTEST gpu.gm20b.b FAIL own_failed=1 own_why=enc.pass/1 skipped_why=call.blit/1 ms=1.0 why=x', 0)
        newer = {**line('HWTEST suite PASS pass=1 fail=0 skip=0 ms=1.0', 9), 'bootUUID': 'B00T2'}
        results, _ = devicerun.run_checks([older, newer], hwtest.HWTEST)
        rows = {r.check.name: r for r in reports.rolled_up(results)}
        self.assertEqual(rows['hwtest.first-red'].verdict, NO_DATA)
        self.assertIn('boot B00T2', rows['hwtest.first-red'].observed)
        self.assertNotIn('hwtest.red.enc.pass', rows)
        self.assertEqual(rows['hwtest.skipped-calls'].verdict, NO_DATA)


class AppSources(unittest.TestCase):
    @staticmethod
    def declared(directories: tuple[Path, ...]) -> dict[str, bool]:
        ids = {}
        for source in sorted(path for directory in directories for path in directory.glob('*.swift')):
            for block in re.split(r'\n(?=struct |enum )', source.read_text()):
                for test_id in re.findall(r'(?:let id = |\bid: )"([a-z0-9_.]+)"', block):
                    ids[test_id] = 'let interactive = true' in block
        return ids

    def test_every_literal_id_is_checked_with_its_interactivity(self):
        declared = self.declared(SHARED)
        self.assertGreater(len(declared), 40)
        for test_id, interactive in declared.items():
            self.assertIn(test_id, hwtest.TESTS)
            self.assertEqual(hwtest.TESTS[test_id], interactive, test_id)

    def test_every_other_id_is_one_of_the_app_tables(self):
        declared = self.declared(SHARED)
        for test_id in hwtest.TESTS:
            if test_id not in declared:
                self.assertRegex(test_id, PARAMETERIZED)

    def test_each_versions_own_tests_are_its_table(self):
        shared = self.declared(SHARED)
        for ios, directories in BY_IOS.items():
            declared = self.declared(directories)
            # A version's own form of a shared test keeps the shared id and row; its app leaves the shared one out
            sources = ''.join(path.read_text() for directory in directories for path in directory.glob('*.swift'))
            for test_id in set(declared) & set(shared):
                self.assertEqual(declared.pop(test_id), shared[test_id], test_id)
                self.assertIn(f'$0.id != "{test_id}"', sources, test_id)
            self.assertEqual(declared, hwtest.BY_IOS.get(ios, {}), ios)
            self.assertFalse(set(declared) & set(hwtest.TESTS), ios)


# A sysctl name a test reads: a literal "kern.x"/"debug.x", or such a prefix joined to each of the file's
# `static let counters` (CompletionFlipTest's "kern.switch_flips_" + name).
SYSCTL_NAME = re.compile(r'"((?:kern|debug)\.[a-z0-9_.]+)"(\s*\+)?')
COUNTERS = re.compile(r'static let counters = \[([^\]]*)\]')


class SandboxSysctls(unittest.TestCase):
    def test_every_sysctl_a_test_reads_is_in_both_sandbox_exceptions(self):
        names = set()
        for directory in SHARED + tuple(path for paths in BY_IOS.values() for path in paths):
            for source in directory.glob('*.swift'):
                text = source.read_text()
                counters = [name for block in COUNTERS.findall(text) for name in re.findall(r'"([a-z0-9_]+)"', block)]
                for match in SYSCTL_NAME.finditer(text):
                    if match.group(2):
                        names.update(match.group(1) + name for name in counters)
                    else:
                        names.add(match.group(1))
        self.assertGreater(len(names), 20)
        for entitlements in ('SwitchHWTest.entitlements', 'SwitchHWTest27.entitlements'):
            plist = plistlib.loads((APP / entitlements).read_bytes())
            listed = set(plist['com.apple.security.exception.sysctl.read-only'])
            self.assertEqual(sorted(names - listed), [], entitlements)


# A literal first argument of Metrics.set: "key", prefix + "key" or an interpolated "\\(label)_key".
METRIC_SET = re.compile(r'\.set\(\s*(?:[A-Za-z_][\w.]*\s*\+\s*)?"')


def swift_string(text: str, start: int) -> tuple[str, bool, int]:
    """Return a literal's text outside interpolations (it opens at text[start]), whether it interpolates, its end."""
    parts, at, interpolated = [], start + 1, False
    while text[at] != '"':
        if text.startswith('\\(', at):
            interpolated, depth, at = True, 1, at + 2
            while depth:
                if text[at] == '"':
                    at = swift_string(text, at)[2]
                    continue
                depth += {'(': 1, ')': -1}.get(text[at], 0)
                at += 1
        elif text[at] == '\\':
            parts.append(text[at + 1])
            at += 2
        else:
            parts.append(text[at])
            at += 1
    return ''.join(parts), interpolated, at + 1


class MetricKeys(unittest.TestCase):
    def test_no_literal_metric_key_is_reserved_or_malformed(self):
        reserved = {'hw', 'ms', 'why'}  # LogLine.reservedKeys
        bad = []
        for source in sorted(APP.rglob('*.swift')):
            if source.relative_to(APP).parts[0].startswith('host_'):
                continue
            text = source.read_text()
            for match in METRIC_SET.finditer(text):
                key, interpolated, _ = swift_string(text, match.end() - 1)
                whole = not interpolated and '+' not in match.group(0)
                if (whole and key in reserved) or not re.fullmatch(r'[a-z0-9_]*', key):
                    bad.append(f'{source.relative_to(APP)}:{text.count(chr(10), 0, match.start()) + 1} {key!r}')
        self.assertEqual(bad, [], 'metric keys the log line cannot carry (LogLine.isValidKey, reservedKeys)')

    def test_the_scan_sees_through_interpolations(self):
        text = 'm.set("err_\\(e < 0 ? "neg" : "pos")_ms", 1); m.set(prefix + "frames", 2); m.set("ms", 3)'
        found = [swift_string(text, match.end() - 1)[:2] for match in METRIC_SET.finditer(text)]
        self.assertEqual(found, [('err__ms', True), ('frames', False), ('ms', False)])

    def test_no_arm_name_is_malformed(self):
        bad = []
        for source in sorted(APP.rglob('*.swift')):
            if source.relative_to(APP).parts[0].startswith('host_'):
                continue
            text = source.read_text()
            for name, at in arm_names(text):
                if not re.fullmatch(r'[a-z0-9_]+', name):
                    bad.append(f'{source.relative_to(APP)}:{text.count(chr(10), 0, at) + 1} {name!r}')
        self.assertEqual(bad, [], 'arm names, which tests interpolate into metric keys, the log line cannot carry')

    def test_the_arm_scan_reads_names_and_skips_forms(self):
        text = 'static let arms: [(rung: Int, name: String)] = [(5, "twin1"), (6, "twinW")]\nlet x = ["Up"]'
        self.assertEqual([n for n, _ in arm_names(text)], ['twin1', 'twinW'])
        self.assertEqual([n for n, _ in arm_names('let arms = [("control", "pass=today")]')], ['control'])


# The initializer of a test's arms table (`static let arms ... = [`, `let arms: ... = [`)
ARMS = re.compile(r'\blet arms\b[^=\n]*=\s*\[')


def arm_names(text: str) -> list[tuple[str, int]]:
    """Return the identifier-like literals (an arm's name, a shader's) of every arms table, with their offsets."""
    names = []
    for match in ARMS.finditer(text):
        at, depth = match.end(), 1
        while depth and at < len(text):
            if text[at] == '"':
                literal, interpolated, end = swift_string(text, at)
                if not interpolated and re.fullmatch(r'[A-Za-z0-9_]+', literal):
                    names.append((literal, at))
                at = end
                continue
            depth += {'[': 1, ']': -1}.get(text[at], 0)
            at += 1
    return names


if __name__ == '__main__':
    unittest.main()
