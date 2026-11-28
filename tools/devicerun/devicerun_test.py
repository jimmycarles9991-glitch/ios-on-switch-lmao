"""tools/devicerun over synthetic `log show --style ndjson` lines and crash reports."""

import contextlib
import io
import re
import unittest
from pathlib import Path

from tools.devicerun import checks, devicerun, hwtest, rules
from tools.devicerun.rules import FAIL, INFO, NO_DATA, PASS, Check

TEST = Path(__file__).resolve().parent / 'test'
NDJSON = TEST / 'device.ndjson'


def run(*check_list: Check) -> dict[str, rules.Result]:
    results, _ = devicerun.run_checks(devicerun.read_ndjson(str(NDJSON)), list(check_list))
    return {r.check.name: r for r in results}


def record(second: float, message: str, level: str = 'Default', boot: str = 'B1') -> dict:
    return {
        'eventType': 'logEvent',
        'timestamp': f'2026-11-14 09:00:{second:09.6f}+0000',
        'bootUUID': boot,
        'processImagePath': '/kernel.development',
        'messageType': level,
        'eventMessage': message,
    }


def judged(records: list[dict], *check_list: Check) -> dict[str, rules.Result]:
    results, _ = devicerun.run_checks(records, list(check_list))
    return {r.check.name: r for r in results}


class ScreenClass(unittest.TestCase):
    """mg.screen-class wants the pinned identity's class: J181's 21 on 27, J120's 11 on 17."""

    @staticmethod
    def verdicts(darwin: int, screen_class: int) -> dict[str, str]:
        group = {c.name: c for c in checks.GROUPS['mobilegestalt']}
        logo = record(2, f'PUIProgressWindow got screen class {screen_class}')
        logo['processImagePath'] = '/usr/libexec/backboardd'
        log = [record(1, f'Darwin Kernel Version {darwin}.0.0: debug'), logo]
        found = judged(log, group['mg.kernel'], group['mg.screen-class'], group['mg.screen-class-17'])
        return {name: found[name].verdict for name in ('mg.screen-class', 'mg.screen-class-17')}

    def test_each_version_wants_its_identitys_class(self):
        self.assertEqual(self.verdicts(25, 21), {'mg.screen-class': PASS, 'mg.screen-class-17': rules.NA})
        self.assertEqual(self.verdicts(23, 11), {'mg.screen-class': rules.NA, 'mg.screen-class-17': PASS})

    def test_the_other_identitys_class_or_none_fails(self):
        self.assertEqual(self.verdicts(25, 11)['mg.screen-class'], FAIL)
        self.assertEqual(self.verdicts(25, -1)['mg.screen-class'], FAIL)
        self.assertEqual(self.verdicts(23, 21)['mg.screen-class-17'], FAIL)


class Checks(unittest.TestCase):
    def test_a_sparse_window_with_a_long_wait_fails_the_longest_wait_check(self):
        """Boot 2693043C (25 Nov): two runs in 10 s, one of them after 9.2 s on a run queue."""
        sched = {c.name: c for c in checks.GROUPS['sched']}
        sparse = 'backboardd.main[69] 31/31 TS DF DF: 2 runs 131072/9169075/9169075 us; render-server[69] 55/55 FX'
        busy = 'backboardd.main[69] 31/31 TS DF DF: 400 runs 64/900/4000 us; render-server[69] 55/55 FX'
        log = [record(t, f'SwitchSched 1/2 (10000 ms): {line}') for t, line in ((1.0, busy), (11.0, sparse))]
        got = judged(log, sched['sched.backboardd-main'], sched['sched.backboardd-main-longest'])
        self.assertEqual(got['sched.backboardd-main'].verdict, PASS)
        self.assertEqual(got['sched.backboardd-main-longest'].verdict, FAIL)

    def test_host1x_channel_counters_that_never_started_fail(self):
        media = {c.name: c for c in checks.GROUPS['media']}
        head = 'TegraHost1x: 5s NVDEC/VIC/NVENC/NVJPG/DC gathered=0/40/0/0/60 copied=0/0/0/0/0 arena_high=0/3/0/0/1KB '
        running = head + 'room_waits=0/0/0/0/0 clocked=0/5000000/0/0/5000000us stalled=0/1000000/0/0/4500000us'
        stopped = head + 'room_waits=0/0/0/0/0 clocked=0/0/0/0/0us stalled=0/0/0/0/0us'
        check = media['media.host1x-channel-counters']
        self.assertEqual(judged([record(1.0, running)], check)[check.name].verdict, PASS)
        self.assertEqual(judged([record(1.0, running), record(6.0, stopped)], check)[check.name].verdict, FAIL)
        self.assertEqual(
            judged([record(1.0, head + 'room_waits=0/0/0/0/0 counters=off')], check)[check.name].verdict, NO_DATA
        )

    def test_every_check_is_well_formed(self):
        names = [c.name for group in checks.GROUPS.values() for c in group]
        self.assertEqual(len(names), len(set(names)))
        for group in checks.GROUPS.values():
            for c in group:
                self.assertTrue(
                    c.rule in {rules.REPORT, rules.TIMELINE, rules.SUMS} or rules.RULE.match(c.rule), c.name
                )
                self.assertTrue(all(c.literals()), c.name)
                if c.regex:
                    re.compile(c.regex)
                self.assertTrue(all(n in names for n in c.needed()), c.name)
                for other in (c.against, c.after, c.unless, c.followed, c.during, c.per, c.preceded, c.boots):
                    self.assertTrue(not other or other in names, c.name)
                self.assertTrue(all(n in names for n in c.outside), c.name)

    def test_zbc_full_lines_are_one_a_process_and_type(self):
        full = next(c for c in checks.GROUPS['gpu'] if c.name == 'gm20b.zbc-full-lines')

        def record(process: str, pid: int, kind: str) -> dict:
            return {
                'timestamp': '2026-11-22 09:00:00.000000-0800',
                'processImagePath': f'/private/var/containers/Bundle/Application/X/{process}.app/{process}',
                'subsystem': checks.GPU_METAL_SUBSYSTEM,
                'category': 'gm20b',
                'messageType': 'Error',
                'eventMessage': f"SwitchMetal: {process}[{pid}] the GPU's ZBC {kind} table is full (every process's, "
                'never emptied): full clears of compressed targets to values it lacks write their texels',
            }

        log = [
            record('SwitchHWTest27', 412, 'colour'),
            record('SwitchHWTest27', 977, 'colour'),
            record('backboardd', 61, 'colour'),
            record('backboardd', 61, 'depth'),
        ]
        results, _ = devicerun.run_checks(log, [full])
        self.assertEqual(results[0].verdict, PASS, results[0].observed)
        self.assertIn('SwitchHWTest27/977/colour x1', results[0].observed)
        results, _ = devicerun.run_checks([*log, record('backboardd', 61, 'depth')], [full])
        self.assertEqual(results[0].verdict, FAIL)
        self.assertIn('backboardd/61/depth x2', results[0].observed)

    def test_predicate_escapes_and_covers_every_check(self):
        check = Check('q', 'o', 'say "hi" \\ there', process='kernel', subsystem='a.b', level='error')
        self.assertEqual(
            devicerun.clause(check),
            '((eventMessage CONTAINS "say \\"hi\\" \\\\ there") AND process BEGINSWITH "kernel" '
            'AND subsystem CONTAINS "a.b" AND messageType == error)',
        )
        self.assertIn('BEGINSWITH "SwitchFrame"', devicerun.predicate([check]))
        negated = devicerun.clause(Check('n', 'o', 'x', process='!SwitchHWTest'))
        self.assertIn('NOT (process BEGINSWITH "SwitchHWTest")', negated)
        self.assertIn('messageType == error', devicerun.clause(Check('l', 'o', 'x', level='Error')))

    def test_debug_checks_alone_see_info_and_debug_lines(self):
        plain = Check('plain', 'o', 'x', 'count > 0')
        debug = Check('debug', 'o', 'x', 'count > 0', debug=True)
        self.assertNotIn('messageType', devicerun.predicate([plain]))
        query = devicerun.predicate([plain, debug])
        self.assertTrue(query.startswith('((((eventMessage CONTAINS "x")) OR eventMessage BEGINSWITH "SwitchFrame")'))
        self.assertIn('AND messageType != info AND messageType != debug) OR ((eventMessage CONTAINS "x"))', query)
        records = [record(0, 'x', 'Debug')]
        got = {r.check.name: r.verdict for r in devicerun.run_checks(records, [plain, debug])[0]}
        self.assertEqual(got, {'plain': FAIL, 'debug': PASS})


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

    def test_frame_end_idle_leaves_out_debug_only_holders(self):
        check = next(c for c in checks.GROUPS['display'] if c.name == 'display.frame-end-idle')
        locks = 'SwitchDisplay locks: 300 frame ends; per 100: colour 0, INT_MASK 0; work-loop passes 100; '
        debug = locks + '5011 ms, 0 swaps, 302 DC A interrupts, 2 not explained by debug holders, routed for release'
        busy = locks + '5000 ms, 300 swaps, 300 DC A interrupts, 300 not explained by debug holders, routed for release'
        leak = locks + '5000 ms, 0 swaps, 295 DC A interrupts, 295 not explained by debug holders, routed for release'

        def verdict(*messages: str) -> str:
            records = [record(i, m) for i, m in enumerate(messages)]
            return devicerun.run_checks(records, [check])[0][0].verdict

        self.assertEqual(verdict(debug, busy), PASS)
        self.assertEqual(verdict(debug, leak), FAIL)

    def test_transform_refused_names_the_word(self):
        check = next(c for c in checks.GROUPS['display'] if c.name == 'display.transform-refused')
        line = 'SwitchDisplay: swap refused: layer 2 has transform 1 (format 0x42475241, 1280x720)'
        result = devicerun.run_checks([record(0, line)], [check])[0][0]
        self.assertEqual(result.verdict, INFO)
        self.assertIn('1/0x42475241/1280x720', result.observed)

    def test_against_and_grouping(self):
        by_name = {c.name: c for c in checks.GROUPS['gpu'] + checks.GROUPS['refusals']}
        got = run(by_name['gpu.surface-locks'], by_name['gpu.rainbow'], by_name['refusal.user'])
        self.assertEqual(got['gpu.rainbow'].verdict, INFO)
        self.assertIn('2 surfaces, 1 without a gpu.surface-locks line: 78', got['gpu.rainbow'].observed)
        self.assertIn('backboardd/gm20b/format', got['refusal.user'].observed)

    def test_kernel_refusal_line(self):
        refusal = next(c for c in checks.GROUPS['refusals'] if c.name == 'refusal.kernel')
        self.assertIn('kernel_task/SwitchVIC/pitch x1', run(refusal)['refusal.kernel'].observed)

    def test_vic_colour_refusals_name_their_process_and_change(self):
        vic = next(c for c in checks.GROUPS['media'] if c.name == 'media.vic-colour-refusals')
        line = (
            "TegraVIC: backboardd[63] transfer function change (VIC's curves are not Apple's): IOSurfaceAccelerator "
            "transform refused (not on VIC), the caller falls back: '420v' 720x480 to 'BGRA' 1280x720"
        )
        record = {
            'timestamp': '2026-11-19 09:12:00.000000-0800',
            'processImagePath': '/kernel',
            'messageType': 'Error',
            'eventMessage': line,
        }
        results, _ = devicerun.run_checks([record], [vic])
        self.assertEqual(results[0].verdict, INFO)
        self.assertIn('backboardd/transfer function change', results[0].observed)

    def test_vic_pitch_fails_on_a_row_pitch_refusal_only(self):
        pitch = next(c for c in checks.GROUPS['media'] if c.name == 'media.vic-pitch')

        def record(why: str) -> dict:
            return {
                'timestamp': '2026-11-19 09:12:00.000000-0800',
                'processImagePath': '/kernel',
                'messageType': 'Error',
                'eventMessage': f'TegraVIC: mediaserverd[71] {why}: IOSurfaceAccelerator transform refused '
                "(not on VIC), the caller falls back: '420v' 1920x1080 to 'BGRA' 1920x1080",
            }

        results, _ = devicerun.run_checks([record('row pitch the engine cannot address')], [pitch])
        self.assertEqual(results[0].verdict, FAIL)
        results, _ = devicerun.run_checks([record("transfer function change (VIC's curves are not Apple's)")], [pitch])
        self.assertEqual(results[0].verdict, PASS)

    def test_compute_view_refusals(self):
        view = next(c for c in checks.GROUPS['refusals'] if c.name == 'refusal.compute-view')

        def record(message: str, subsystem: str) -> dict:
            return {
                'timestamp': '2026-11-09 15:51:01.210000+0000',
                'bootUUID': 'B00T',
                'processImagePath': '/usr/libexec/PosterBoard',
                'subsystem': subsystem,
                'category': 'gm20b',
                'messageType': 'Error',
                'eventMessage': message,
            }

        boot17 = [
            record('PosterBoard[261] compute.view: a=0 (0x0) b=0 (0x0)', checks.REFUSAL_SUBSYSTEM),
            record(
                'PosterBoard[261] cb.m2v: command buffer on m2v (compute): a kernel m2v binds a view of another '
                'format at a texture slot [first predicate compute.view]',
                checks.REFUSAL_SUBSYSTEM,
            ),
            record('TegraGM20BMetal: PosterBoard raw 5s why=compute.view:7/7', checks.GPU_METAL_SUBSYSTEM),
        ]
        results, _ = devicerun.run_checks(boot17, [view])
        self.assertEqual(results[0].verdict, FAIL)
        self.assertIn('PosterBoard', results[0].observed)
        results, _ = devicerun.run_checks(boot17[2:], [view])
        self.assertEqual(results[0].verdict, PASS)

    def test_a_failed_transfers_unreached_line_is_counted_per_run(self):
        runs = next(c for c in hwtest.HWTEST if c.name == hwtest.TRANSFER_FAILED_RUNS)
        lines = next(c for c in checks.GROUPS['refusals'] if c.name == 'refusal.texture-unreached-hwtest')

        def record(second: int, subsystem: str, message: str) -> dict:
            return {
                'timestamp': f'2026-11-21 09:00:{second:02d}.000000-0800',
                'bootUUID': 'B00T',
                'processImagePath': '/Applications/SwitchHWTest.app/SwitchHWTest',
                'subsystem': subsystem,
                'category': 'gm20b',
                'messageType': 'Error',
                'eventMessage': message,
            }

        unreached = (
            'SwitchHWTest[512] texture.cpu.unreached: replaceRegion of 16x16 at (0, 0) of block-linear IOSurface 77: '
            "the copy engine's transfer failed: nothing moved"
        )
        run = 'HWTEST gpu.gm20b.texture_transfer_failed PASS armed_before=0 fired_write=1 ms=2000.0'
        log = [
            record(1, checks.REFUSAL_SUBSYSTEM, unreached),
            {**record(2, hwtest.SUBSYSTEM, run), 'category': 'hwtest', 'messageType': 'Default'},
            {**record(8, hwtest.SUBSYSTEM, run), 'category': 'hwtest', 'messageType': 'Default'},
        ]
        got = {r.check.name: r for r in devicerun.run_checks(log, [runs, lines])[0]}
        self.assertEqual(got[lines.name].verdict, FAIL)
        self.assertIn('2 runs, 1 off', got[lines.name].observed)
        got = {r.check.name: r for r in devicerun.run_checks(log[:2], [runs, lines])[0]}
        self.assertEqual(got[lines.name].verdict, PASS)

    def test_the_file_indexer_failing_inside_a_hwtest_run_fails_and_outside_it_does_not(self):
        runs = next(c for c in hwtest.HWTEST if c.name == hwtest.RUN_WINDOWS)
        lines = next(c for c in checks.GROUPS['storage'] if c.name == 'storage.hwtest-indexed-mid-write')

        def record(second: int, image: str, subsystem: str, message: str) -> dict:
            return {
                'timestamp': f'2026-11-22 23:{second // 60:02d}:{second % 60:02d}.000000-0800',
                'bootUUID': 'B00T',
                'processImagePath': image,
                'subsystem': subsystem,
                'category': 'daemon',
                'messageType': 'Error',
                'eventMessage': message,
            }

        indexer = (
            '/System/Library/PrivateFrameworks/FileIndexer.framework/fileindexerd',
            'com.apple.FileIndexer',
            'failed to get metadata for (12, in: 3): NSError: POSIX 2 '
            '"The operation couldn’t be completed. No such file or directory"',
        )
        suite = 'HWTEST suite FAIL pass=600 fail=40 skip=28 ios=27 ms=2045000.0'
        app = '/Applications/SwitchHWTest.app/SwitchHWTest'
        log = [
            record(300, *indexer),
            {**record(2200, app, hwtest.SUBSYSTEM, suite), 'category': 'results', 'messageType': 'Default'},
            record(2300, *indexer),
        ]
        got = {r.check.name: r for r in devicerun.run_checks(log, [runs, lines])[0]}
        self.assertEqual(got[lines.name].verdict, FAIL)
        self.assertIn('1 runs, 1 off', got[lines.name].observed)
        got = {r.check.name: r for r in devicerun.run_checks(log[1:], [runs, lines])[0]}
        self.assertEqual(got[lines.name].verdict, PASS)

    def test_a_title_scroll_repetition_past_the_first_takes_no_fresh_import(self):
        reps, imports = (
            next(c for c in checks.GROUPS['refusals'] if c.name == name)
            for name in (checks.TITLE_SCROLL_REPS, 'compositor.title-scroll-imports')
        )

        def record(second: float, proc: str, subsystem: str, message: str) -> dict:
            return {
                'timestamp': f'2026-11-23 09:00:{second:06.3f}-0800',
                'bootUUID': 'B00T',
                'processImagePath': f'/usr/libexec/{proc}',
                'subsystem': subsystem,
                'category': 'marks',
                'messageType': 'Default',
                'eventMessage': message,
            }

        user = 'SwitchFrameUser: backboardd cb 9 commit 1 enc 1 1 replay 0 0 ret 0 owait 0 0 fence 2 done 2 back 0 '
        log = [
            record(
                1.2, 'backboardd', checks.GPU_METAL_SUBSYSTEM, user + 'slot 0 0 prev 0 dropped 0 surf 1 imp 2 hit 0'
            ),
            record(
                1.4, 'SwitchHWTest27', 'com.brunostjohn.switch.hwtest', 'SwitchMark: ui.title_scroll.paused.0 500 ms'
            ),
            record(
                3.2, 'backboardd', checks.GPU_METAL_SUBSYSTEM, user + 'slot 0 0 prev 0 dropped 0 surf 0 imp 0 hit 3'
            ),
            record(
                3.4, 'SwitchHWTest27', 'com.brunostjohn.switch.hwtest', 'SwitchMark: ui.title_scroll.paused.1 500 ms'
            ),
        ]
        got = {r.check.name: r for r in devicerun.run_checks(log, [reps, imports])[0]}
        self.assertEqual(got[imports.name].verdict, PASS)
        self.assertIn('1 runs, 0 off', got[imports.name].observed)
        log[0] = {**log[0], 'timestamp': '2026-11-23 09:00:03.100-0800'}
        got = {r.check.name: r for r in devicerun.run_checks(log, [reps, imports])[0]}
        self.assertEqual(got[imports.name].verdict, FAIL)

    def test_a_promise_given_up_after_its_makers_kill_fails(self):
        judged = [c for c in checks.GROUPS['gpu'] if c.name.startswith(('gpu.channel-killed', 'gpu.promise-given-up'))]

        def kernel(second: int, message: str) -> dict:
            return {
                'timestamp': f'2026-11-21 09:00:{second:02d}.000000-0800',
                'bootUUID': 'B00T',
                'processImagePath': '/kernel',
                'subsystem': '',
                'category': '',
                'messageType': 'Default',
                'eventMessage': message,
            }

        killed = kernel(10, 'tegra-gm20b: GPU error in Weather (channel 5): channel killed, GPU running: hang: x')

        def given_up(second: int, maker: str) -> dict:
            return kernel(
                second,
                f"tegra-gm20b: {maker}'s writer promise at fence offset 256 (value 9) was not kept: backboardd waited "
                '1000 ms for it in a composite; it goes on without the promised writes',
            )

        def verdicts(*log: dict) -> dict[str, str]:
            return {r.check.name: r.verdict for r in devicerun.run_checks(list(log), judged)[0]}

        self.assertEqual(verdicts(killed, given_up(11, 'Weather'))['gpu.promise-given-up-after-kill'], FAIL)
        self.assertEqual(verdicts(killed, given_up(11, 'Maps'))['gpu.promise-given-up-after-kill'], PASS)
        self.assertEqual(verdicts(killed, given_up(13, 'Weather'))['gpu.promise-given-up-after-kill'], PASS)
        self.assertEqual(verdicts(given_up(9, 'Weather'), killed)['gpu.promise-given-up-after-kill'], PASS)
        self.assertEqual(verdicts(killed, given_up(11, 'Maps'))['gpu.promise-given-up'], INFO)

    def test_compositor_foreign_waits_excuses_only_promises_in_promise_unkept_windows(self):
        judged = [
            c for c in checks.GROUPS['gpu'] if c.name in {checks.PROMISE_UNKEPT_WINDOWS, 'gpu.compositor-foreign-waits'}
        ]

        def record(second: int, message: str, path: str = '/kernel', subsystem: str = '', boot: str = 'B00T') -> dict:
            return {
                'timestamp': f'2026-11-23 09:00:{second:02d}.000000-0800',
                'bootUUID': boot,
                'processImagePath': path,
                'subsystem': subsystem,
                'category': '',
                'messageType': 'Default',
                'eventMessage': message,
            }

        def stats(
            second: int,
            jobs: int,
            fences: int,
            promises: int,
            events: int,
            host1x: int = 0,
            boot: str = 'B00T',
        ) -> dict:
            return record(
                second,
                f'tegra-gm20b: {jobs} compositor jobs waited on foreign work ({fences} fences, '
                f'{promises} promises, {events} events; {host1x} host1x waits), queue mean 100 us max 200 us ({jobs})',
                boot=boot,
            )

        def verdict(*log: dict) -> str:
            got = {r.check.name: r.verdict for r in devicerun.run_checks(list(log), judged)[0]}
            return got['gpu.compositor-foreign-waits']

        promise_unkept_test = record(
            15,
            'HWTEST gpu.path.promise_unkept PASS d_abandoned=1 waited_ms=5002 ms=5200.0',
            path='/Applications/SwitchHWTest.app/SwitchHWTest',
            subsystem=hwtest.SUBSYSTEM,
        )
        promise_unkept_bare = record(
            15,
            'HWTEST gpu.path.promise_unkept PASS ms=5200.0',
            path='/Applications/SwitchHWTest.app/SwitchHWTest',
            subsystem=hwtest.SUBSYSTEM,
        )
        present_promise_test = record(
            15,
            'HWTEST gpu.path.present_promise PASS flags=0x1 ms=500.0',
            path='/Applications/SwitchHWTest.app/SwitchHWTest',
            subsystem=hwtest.SUBSYSTEM,
        )
        promise_given_up = record(
            15,
            "tegra-gm20b: SwitchHWTest's writer promise at fence offset 256 (value 9) was not kept: "
            'backboardd waited 5000 ms for it in a composite; it goes on without the promised writes',
        )
        production_given_up = record(
            15,
            "tegra-gm20b: MobileSafari's writer promise at fence offset 256 (value 9) was not kept: "
            'backboardd waited 5000 ms for it in a composite; it goes on without the promised writes',
        )

        self.assertEqual(verdict(stats(10, 0, 0, 0, 0, host1x=0)), PASS)
        self.assertEqual(verdict(stats(10, 0, 0, 0, 0, host1x=5)), PASS)
        self.assertEqual(verdict(stats(10, 1, 1, 0, 0)), FAIL)
        self.assertEqual(verdict(stats(10, 1, 0, 0, 1)), FAIL)
        self.assertEqual(verdict(stats(12, 1, 1, 0, 0), promise_unkept_test), FAIL)
        self.assertEqual(verdict(stats(12, 1, 0, 0, 1), promise_unkept_test), FAIL)
        self.assertEqual(verdict(stats(12, 2, 1, 1, 0), promise_unkept_test), FAIL)
        self.assertEqual(verdict(stats(12, 1, 0, 1, 0), promise_unkept_test), PASS)
        self.assertEqual(verdict(stats(12, 1, 0, 1, 0), promise_unkept_bare), PASS)
        self.assertEqual(verdict(stats(18, 1, 0, 1, 0), promise_unkept_test), PASS)
        self.assertEqual(verdict(stats(15, 1, 0, 1, 0), present_promise_test), PASS)
        self.assertEqual(verdict(stats(18, 1, 0, 1, 0), present_promise_test), PASS)
        self.assertEqual(verdict(stats(15, 1, 1, 0, 0), present_promise_test), FAIL)
        self.assertEqual(verdict(stats(12, 1, 0, 1, 0), promise_given_up), PASS)
        self.assertEqual(verdict(stats(18, 1, 0, 1, 0), promise_given_up), PASS)
        self.assertEqual(verdict(stats(12, 1, 0, 1, 0), production_given_up), FAIL)
        self.assertEqual(verdict(stats(30, 1, 0, 1, 0), promise_unkept_test), FAIL)
        self.assertEqual(verdict(stats(30, 1, 0, 1, 0), promise_given_up), FAIL)
        self.assertEqual(verdict(stats(12, 1, 0, 1, 0)), FAIL)
        self.assertEqual(verdict(stats(12, 1, 0, 1, 0, boot='B00T2'), promise_unkept_test), FAIL)

    def test_a_refused_vic_read_and_an_old_vblank_phase_line_fail(self):
        judged = [c for g in ('gpu', 'display') for c in checks.GROUPS[g] if c.owner == 'lost-work-3']

        def kernel(message: str) -> dict:
            return {
                'timestamp': '2026-11-21 09:00:00.000000-0800',
                'bootUUID': 'B00T',
                'processImagePath': '/kernel',
                'subsystem': '',
                'category': '',
                'messageType': 'Default',
                'eventMessage': message,
            }

        def verdicts(*messages: str) -> dict[str, str]:
            return {r.check.name: r.verdict for r in devicerun.run_checks([kernel(m) for m in messages], judged)[0]}

        clean = verdicts('SwitchDisplay stats: 1000 ms: 60 DC A interrupts, 60 swaps')
        self.assertEqual(set(clean.values()), {PASS})
        refused = verdicts(
            'tegra-gm20b: MC refused a VIC read at 1001d400: VIC read mapping 10000000 of 30 pages: 0x1400 past its '
            'last page',
            'tegra-gm20b: PBDMA0 00000000 ch 0 mthd 00000000 data 00000000, PMC 00000000, MC 00000000 err 6000006c at '
            '1001d400',
        )
        self.assertEqual(refused['gpu.vic-read-refused'], FAIL)
        self.assertEqual(verdicts('SwitchDisplay: V_PULSE2 never reached the CPU')['display.vblank-phase-gone'], FAIL)

    def test_a_probe_row_runs_kills_are_its_killed_rows_class_errors(self):
        runs = {c.name: c for c in hwtest.HWTEST}
        lines = {c.name: c for c in checks.GROUPS['gpu']}

        def record(second: int, path: str, subsystem: str, message: str) -> dict:
            return {
                'timestamp': f'2026-11-23 09:00:{second:02d}.000000-0800',
                'bootUUID': 'B00T',
                'processImagePath': path,
                'subsystem': subsystem,
                'category': 'results',
                'messageType': 'Default',
                'eventMessage': message,
            }

        def kill(second: int, code: str, method: str, data: str) -> dict:
            return record(
                second,
                '/System/Library/Kernels/kernel.development',
                '',
                f'tegra-gm20b: GPU error in SwitchHWTest (channel 18): channel killed, GPU running: gr: DATA_ERROR '
                f'{code} [] ch 18 [00f11f4000 SwitchHWTest] subc 0 class b197 mthd {method} data {data} | fifo:0',
            )

        def run(second: int, killed: int) -> dict:
            return record(
                second,
                '/Applications/SwitchHWTest.app/SwitchHWTest',
                hwtest.SUBSYSTEM,
                f'HWTEST gpu.gm20b.mixed_end_rows PASS row_end_mode=killed killed={killed} ms=4000.0',
            )

        log = [kill(1, '00000004', '0fb8', '00000000'), kill(2, '00000127', '0d78', '00000004'), run(3, 2)]
        check = [runs[hwtest.TIR_ROWS_RUNS], lines['gpu.tir-rows-errors']]
        got = {r.check.name: r for r in devicerun.run_checks(log, check)[0]}
        self.assertEqual(got['gpu.tir-rows-errors'].verdict, PASS)
        self.assertEqual([m['code'] for m in got['gpu.tir-rows-errors'].matches], ['00000004', '00000127'])
        got = {r.check.name: r for r in devicerun.run_checks(log[1:], check)[0]}
        self.assertEqual(got['gpu.tir-rows-errors'].verdict, FAIL)

    def test_decode_lines_are_counted_per_run_against_each_runs_want(self):
        runs = {c.name: c for c in hwtest.HWTEST}
        lines = {c.name: c for c in checks.GROUPS['refusals']}

        def record(second: int, subsystem: str, message: str) -> dict:
            return {
                'timestamp': f'2026-11-22 09:00:{second:02d}.000000-0800',
                'bootUUID': 'B00T',
                'processImagePath': '/Applications/SwitchHWTest.app/SwitchHWTest',
                'subsystem': subsystem,
                'category': 'gm20b' if subsystem == checks.REFUSAL_SUBSYSTEM else 'results',
                'messageType': 'Error' if subsystem == checks.REFUSAL_SUBSYSTEM else 'Default',
                'eventMessage': message,
            }

        def refusal(second: int, text: str) -> dict:
            return record(second, checks.REFUSAL_SUBSYSTEM, 'SwitchHWTest[812] ' + text)

        readback = [
            refusal(1, 'tex.decoded_readback: getBytes of a format 166 texture: nothing moved (its blocks were ...'),
            refusal(2, 'tex.decoded_readback: copyFromTexture:toBuffer: of a format 164 texture: nothing moved (...'),
            record(3, hwtest.SUBSYSTEM, 'HWTEST gpu.decode.readback PASS decode_readback_lines_want=2 ms=2500.0'),
            record(9, hwtest.SUBSYSTEM, 'HWTEST gpu.decode.readback PASS decode_readback_lines_want=0 ms=2500.0'),
        ]
        check = [runs[hwtest.DECODE_READBACK_RUNS], lines['refusal.tex-decoded-readback-hwtest']]
        got = {r.check.name: r for r in devicerun.run_checks(readback, check)[0]}
        self.assertEqual(got['refusal.tex-decoded-readback-hwtest'].verdict, PASS)
        got = {r.check.name: r for r in devicerun.run_checks(readback[1:3], check)[0]}
        self.assertEqual(got['refusal.tex-decoded-readback-hwtest'].verdict, FAIL)

        view = 'tex.view: no view of format 222, type 2, levels 1, of a 276x4 texture of format 204: a view ...'
        form = (
            "tex.decoded_form: no view of format 221 of a format 203 texture: nil, as the identity's Metal makes none"
        )
        views = 'HWTEST gpu.decode.views PASS decode_view_lines_want=1 decode_form_lines_want={} ms=1900.0'
        check = [
            runs[hwtest.DECODE_VIEWS_RUNS],
            lines['refusal.tex-view-native-hdr-hwtest'],
            lines['refusal.tex-decoded-form-hwtest'],
        ]
        log17 = [refusal(1, view), refusal(1, form), record(2, hwtest.SUBSYSTEM, views.format(1))]
        got = {r.check.name: r for r in devicerun.run_checks(log17, check)[0]}
        self.assertEqual(got['refusal.tex-view-native-hdr-hwtest'].verdict, PASS)
        self.assertEqual(got['refusal.tex-decoded-form-hwtest'].verdict, PASS)
        log27 = [refusal(1, view), refusal(1, form), record(2, hwtest.SUBSYSTEM, views.format(0))]
        got = {r.check.name: r for r in devicerun.run_checks(log27, check)[0]}
        self.assertEqual(got['refusal.tex-decoded-form-hwtest'].verdict, FAIL)

    def test_failed_command_buffers_are_judged_per_process(self):
        failed = [c for c in checks.GROUPS['refusals'] if c.name.startswith('gm20b.failed')]

        def record(process: str, message: str) -> dict:
            return {
                'timestamp': '2026-11-18 15:51:01.210000+0000',
                'bootUUID': 'B00T',
                'processImagePath': '/usr/libexec/' + process,
                'subsystem': checks.REFUSAL_SUBSYSTEM,
                'category': 'gm20b.failed',
                'messageType': 'Error',
                'eventMessage': message,
            }

        hwtest = record(
            'SwitchHWTest',
            'SwitchHWTest[409] pass.fmt: a command buffer failed (MTLCommandBufferErrorDomain 1): a texture buffer '
            'past 16384 texels as a colour attachment, which an M4 Max fails Internal',
        )
        springboard = record(
            'SpringBoard',
            'SpringBoard[88] none: a command buffer failed (MTLCommandBufferErrorDomain 1): A render pipeline could '
            'not be compiled',
        )
        results, _ = devicerun.run_checks([hwtest], failed)
        got = {r.check.name: r for r in results}
        self.assertEqual(got['gm20b.failed'].verdict, PASS)
        self.assertIn('pass.fmt/1', got['gm20b.failed-hwtest'].observed)
        results, _ = devicerun.run_checks([hwtest, springboard], failed)
        got = {r.check.name: r for r in results}
        self.assertEqual(got['gm20b.failed'].verdict, FAIL)
        self.assertIn('SpringBoard/none/1', got['gm20b.failed'].observed)

    def test_an_os_state_the_pack_lacks_fails_the_boot_by_its_label(self):
        covers = [c for c in checks.GROUPS['boot'] if c.name == 'boot.pack-covers-os']

        def record(process: str, message: str) -> dict:
            return {
                'timestamp': '2026-11-23 10:00:01.000000+0000',
                'bootUUID': 'B00T',
                'processImagePath': '/usr/libexec/' + process,
                'subsystem': checks.GPU_METAL_SUBSYSTEM,
                'category': '',
                'messageType': 'Default',
                'eventMessage': message,
            }

        key = 'c0=80 | v fixed_vert_lph_spc | f fixed_frag_lph_cph | l QuartzCore'
        stored = [
            record(
                'backboardd',
                "SwitchMetal: a state the pack lacks made in 3 ms (UI, from the shader compiler's store; queue -1 us) "
                f'after 0 ms for a compile slot: 3ef3a99e4f382ce1 {key}',
            ),
            record(
                'backboardd',
                'SwitchMetal: harvest 3ef3a99e4f382ce1 1/1 '
                f'label=com.apple.coreanimation.PBGRAXmw_TimgA3Xhf_IxrgN3Oc3mtc4nlnlnl {key}',
            ),
        ]
        hwtest = record(
            'SwitchHWTest', f'SwitchMetal: harvest 0123456789abcdef 1/1 label=com.apple.coreanimation.x {key}'
        )
        results, _ = devicerun.run_checks([hwtest], covers)
        self.assertEqual(results[0].verdict, PASS)
        results, _ = devicerun.run_checks([*stored, hwtest], covers)
        self.assertEqual(results[0].verdict, FAIL)
        self.assertEqual(len(results[0].matches), 1)
        self.assertIn('backboardd/PBGRAXmw_TimgA3Xhf_IxrgN3Oc3mtc4nlnlnl', results[0].observed)
        in_process = record(
            'backboardd',
            'SwitchMetal: in-process state the pack lacks 3ef3a99e4f382ce1 (record): '
            'label=com.apple.coreanimation.PBGRABsovXmw_TlrpA3Xhfs_Iclr_Iclr',
        )
        hwtest_in_process = record(
            'SwitchHWTest',
            'SwitchMetal: in-process state the pack lacks 0123456789abcdef (record): '
            'label=com.apple.coreanimation.PBGRAXm_TimgA2Xhf_Isrc',
        )
        results, _ = devicerun.run_checks([hwtest_in_process], covers)
        self.assertEqual(results[0].verdict, PASS)
        results, _ = devicerun.run_checks([in_process, hwtest_in_process], covers)
        self.assertEqual(results[0].verdict, FAIL)
        self.assertIn('backboardd/PBGRABsovXmw_TlrpA3Xhfs_Iclr_Iclr', results[0].observed)

    def test_no_process_draws_the_catch_all_across_the_boot(self):
        rule = [c for c in checks.GROUPS['boot'] if c.name == 'boot.ca-no-catch-all']
        self.assertEqual(len(rule), 1)

        def rates(process: str, cag: int, second: int) -> dict:
            return {
                'timestamp': f'2026-11-23 10:00:{second:02d}.000000+0000',
                'bootUUID': 'B00T',
                'processImagePath': '/usr/libexec/' + process,
                'subsystem': checks.GPU_METAL_SUBSYSTEM,
                'category': 'plugin',
                'messageType': 'Default',
                'eventMessage': f'TegraGM20BMetal: {process} gm20b 5s c=120(24.0/s) pack=45/0/1/0 cag={cag} rtsync=12/345us',
            }

        results, _ = devicerun.run_checks([], rule)
        self.assertEqual(results[0].verdict, NO_DATA)
        clean = [rates('backboardd', 0, 5), rates('backboardd', 0, 10), rates('SwitchHWTest', 0, 11)]
        results, _ = devicerun.run_checks(clean, rule)
        self.assertEqual(results[0].verdict, PASS)
        results, _ = devicerun.run_checks([*clean, rates('backboardd', 3, 15)], rule)
        self.assertEqual(results[0].verdict, FAIL)
        self.assertIn('max(cag) 3', results[0].observed)
        results, _ = devicerun.run_checks([*clean, rates('SpringBoard', 7, 16)], rule)
        self.assertEqual(results[0].verdict, FAIL)
        self.assertIn('max(cag) 7', results[0].observed)
        results, _ = devicerun.run_checks([*clean, rates('SwitchHWTest', 7, 17)], rule)
        self.assertEqual(results[0].verdict, PASS)

    def test_a_core_animation_state_any_process_lacks_fails_the_boot(self):
        rule = [c for c in checks.GROUPS['boot'] if c.name == 'boot.ca-pack-covers-all']
        self.assertEqual(len(rule), 1)

        def record(process: str, message: str) -> dict:
            return {
                'timestamp': '2026-11-25 19:18:55.559000+0000',
                'bootUUID': 'B00T',
                'processImagePath': '/Applications/' + process,
                'subsystem': checks.GPU_METAL_SUBSYSTEM,
                'category': 'gm20b',
                'messageType': 'Error',
                'eventMessage': message,
            }

        own = record(
            'SwitchHWTest',
            'SwitchMetal: SwitchHWTest: a render state the pack lacks f98fae52bc0cfe8f '
            '(com.apple.coreanimation.Pb3a8Xm_Tds4A2Xhf_Isrc): samples=1 c0=94',
        )
        results, _ = devicerun.run_checks([own], rule)
        self.assertEqual(results[0].verdict, PASS)
        for message in (
            'SwitchMetal: Maps: a render state the pack lacks f98fae52bc0cfe8f '
            '(com.apple.coreanimation.PBGRAXm_TdfgA2Xhf_Iscd): samples=1 c0=80',
            'SwitchMetal: in-process state the pack lacks f98fae52bc0cfe8f (record): '
            'label=com.apple.coreanimation.PBGRAXm_TdfgA2Xhf_Iscd',
        ):
            results, _ = devicerun.run_checks([own, record('Maps', message)], rule)
            self.assertEqual(results[0].verdict, FAIL)
            self.assertIn('PBGRAXm_TdfgA2Xhf_Iscd', results[0].observed)

    def test_pack_tier1_uses_reports_os_states_and_ignores_hwtest(self):
        rule = [c for c in checks.GROUPS['boot'] if c.name == 'pack.tier1-uses']
        self.assertEqual(len(rule), 1)
        self.assertIn('logharvest', rule[0].note)

        def line(process: str, name: str, pieces: int, card: int) -> dict:
            return {
                'timestamp': '2026-11-23 10:00:05.000000+0000',
                'bootUUID': 'B00T',
                'processImagePath': '/usr/libexec/' + process,
                'subsystem': checks.GPU_METAL_SUBSYSTEM,
                'category': '',
                'messageType': 'Default',
                'eventMessage': (
                    f'SwitchMetal: tier-1 pack state 0123456789abcdef {name} in {process}: {pieces} pieces, card={card}'
                ),
            }

        hwtest = line('SwitchHWTest', 'aaaa1111bbbb2222', 2, 1)
        results, _ = devicerun.run_checks([hwtest], rule)
        self.assertEqual(results[0].verdict, NO_DATA)
        os_use = [line('backboardd', 'deadbeef00000001', 2, 3), line('SpringBoard', 'deadbeef00000002', 1, 0), hwtest]
        results, _ = devicerun.run_checks(os_use, rule)
        self.assertEqual(results[0].verdict, INFO)
        self.assertEqual(len(results[0].matches), 2)
        self.assertIn('backboardd/deadbeef00000001/2/3', results[0].observed)
        self.assertIn('SpringBoard/deadbeef00000002/1/0', results[0].observed)

    def test_boot_pack_pieces_fails_on_refusal_slow_import_or_piece_import(self):
        rule = [c for c in checks.GROUPS['boot'] if c.name == 'boot.pack-pieces']
        self.assertEqual(len(rule), 1)

        def rec(path: str, subsystem: str, message: str, second: int = 10) -> dict:
            return {
                'timestamp': f'2026-11-24 10:00:{second:02d}.000000+0000',
                'bootUUID': 'B00T',
                'processImagePath': path,
                'subsystem': subsystem,
                'category': 'gm20b',
                'messageType': 'Default',
                'eventMessage': message,
            }

        healthy = [
            rec(
                '/kernel.development',
                '',
                'tegra-gm20b: pack image: tier 0 124 MiB for cameracaptured, open 600 ms after the build began '
                '(tables read), streamed 300 ms after that (pagein requests 125, copy 14 ms, big-page bytes '
                '130023424); 0 jobs waited on it on the GPU, the first submitted 0 ms before its end',
                1,
            ),
            rec(
                '/usr/libexec/backboardd',
                checks.GPU_METAL_SUBSYSTEM,
                'SwitchMetal: backboardd: pack window mapped in 140 us (124 MiB tier 0)',
                2,
            ),
            rec(
                '/usr/libexec/backboardd',
                checks.GPU_METAL_SUBSYSTEM,
                'SwitchMetal: pack window: 5 MiB of CPU ranges wired in 42 ms (pageins 310)',
                3,
            ),
            rec(
                '/kernel.development',
                '',
                'tegra-gm20b: slow import backboardd 3840 KiB surface 42: 6500 us = wire 6000',
                4,
            ),
            rec(
                '/kernel.development',
                '',
                'tegra-gm20b: slow import SwitchHWTest 64 KiB read-only surface 0: 9500 us = wire 9000',
                5,
            ),
        ]
        results, _ = devicerun.run_checks(healthy, rule)
        self.assertEqual(results[0].verdict, PASS, results[0].observed)

        bad_cases = [
            (
                rec('/kernel.development', '', 'tegra-gm20b: pack image: refused (open failed, err 2)'),
                'refused (open failed, err 2)',
            ),
            (
                rec(
                    '/kernel.development',
                    '',
                    'tegra-gm20b: slow import backboardd 64 KiB read-only surface 0: 21000 us = wire 20500',
                ),
                '64 KiB read-only surface 0',
            ),
            (
                rec(
                    '/usr/libexec/backboardd',
                    checks.GPU_METAL_SUBSYSTEM,
                    'SwitchMetal: backboardd: pack image refused (status 2): using per-piece placement',
                ),
                'image refused (status 2)',
            ),
            (
                rec(
                    '/usr/libexec/backboardd',
                    checks.GPU_METAL_SUBSYSTEM,
                    'SwitchMetal: backboardd: pack image map failed (0xe00002c2): using per-piece placement',
                ),
                'image map failed (0xe00002c2)',
            ),
            (
                rec(
                    '/usr/libexec/backboardd',
                    checks.GPU_METAL_SUBSYSTEM,
                    'SwitchMetal: backboardd: pack piece import: tier 0, 1 piece(s) at heap 0x0',
                ),
                'piece import: tier 0',
            ),
        ]
        for bad, expected_detail in bad_cases:
            results, _ = devicerun.run_checks([*healthy, bad], rule)
            self.assertEqual(results[0].verdict, FAIL, expected_detail)
            self.assertIn(expected_detail, results[0].observed)

    def test_the_setup_before_launchd_is_timed_on_boots_that_sync_nothing(self):
        rows = [c for c in checks.GROUPS['boot'] if c.name in {'boot.prefs-before-launchd', 'boot.prefs-synced'}]

        def record(boot: str, tail: str) -> dict:
            return {
                'timestamp': '2026-11-23 10:00:05.000000+0000',
                'bootUUID': boot,
                'processImagePath': '/kernel',
                'messageType': 'Default',
                'eventMessage': f'Switch: persistent Data launchd preferences ready {tail}',
            }

        ordinary = record('B00T', '(1 written, 9 unchanged, 0 durable, 84 ms)')
        panicked = record(
            'B00T2', '(4 written, 8 unchanged, 3 durable, 1412 ms): synced from /private/var/Switch/panic-x.log'
        )
        results, _ = devicerun.run_checks([ordinary, panicked], rows)
        got = {r.check.name: r for r in results}
        self.assertEqual(got['boot.prefs-before-launchd'].verdict, PASS, got['boot.prefs-before-launchd'].observed)
        self.assertEqual(got['boot.prefs-synced'].verdict, INFO)
        self.assertIn('/private/var/Switch/panic-x.log/3/1412', got['boot.prefs-synced'].observed)
        results, _ = devicerun.run_checks([panicked], rows)
        got = {r.check.name: r for r in results}
        self.assertEqual(got['boot.prefs-before-launchd'].verdict, NO_DATA)
        results, _ = devicerun.run_checks([record('B00T', '(1 written, 9 unchanged, 0 durable, 1487 ms)')], rows)
        got = {r.check.name: r for r in results}
        self.assertEqual(got['boot.prefs-before-launchd'].verdict, FAIL)
        self.assertEqual(got['boot.prefs-synced'].verdict, NO_DATA)

    def test_kills_in_a_kill_tests_window_are_that_tests(self):
        judged = [c for c in checks.GROUPS['refusals'] if c.name in {'gm20b.failed', 'gm20b.killed'}]
        windows = [c for c in checks.GROUPS['hwtest'] if c.name == 'hwtest.kill-windows']

        def record(second: int, process: str, message: str, boot: str = 'B00T') -> dict:
            subsystem = 'com.brunostjohn.switch.hwtest' if message.startswith('HWTEST') else checks.REFUSAL_SUBSYSTEM
            if message.startswith('SwitchMetal: '):
                subsystem = checks.GPU_METAL_SUBSYSTEM
            return {
                'timestamp': f'2026-11-18 15:51:{second:02d}.000000+0000',
                'bootUUID': boot,
                'processImagePath': '/usr/libexec/' + process,
                'subsystem': subsystem,
                'category': 'gm20b',
                'messageType': 'Error',
                'eventMessage': message,
            }

        killed = (
            'SpringBoard[88] none: a command buffer failed (MTLCommandBufferErrorDomain 2): The operation '
            'couldn\u2019t be completed. (MTLCommandBufferErrorDomain error 2.)'
        )
        faulted = (
            'WebContent[93] none: a command buffer failed (MTLCommandBufferErrorDomain 3): Caused GPU Address Fault '
            'Error (0000000b:kIOGPUCommandBufferCallbackErrorPageFault)'
        )
        unexplained = 'backboardd[61] none: a command buffer failed (MTLCommandBufferErrorDomain 1): A render pipeline'
        refused = 'backboardd[61] enc.pass: a command buffer failed (MTLCommandBufferErrorDomain 1): a refusal'
        kill = 'SwitchMetal: command buffer 7 failed: the kernel killed its channel (error 4, fault VA 0x0)'
        stall = 'HWTEST gpu.recover.kernel_second_timeout PASS state=4 killed=2 ms=10000.0'
        inside = [record(20, 'SpringBoard', killed), record(21, 'backboardd', refused), record(22, 'PosterBoard', kill)]
        results, _ = devicerun.run_checks(
            [*inside, record(23, 'WebContent', faulted), record(30, 'SwitchHWTest', stall)], judged + windows
        )
        got = {r.check.name: r for r in results}
        self.assertEqual(got['gm20b.failed'].verdict, FAIL)
        self.assertIn('backboardd/enc.pass/1', got['gm20b.failed'].observed)
        self.assertIn(
            'expected: killed by gpu.recover.kernel_second_timeout: SpringBoard/none/2', got['gm20b.failed'].observed
        )
        self.assertIn('WebContent/none/3', got['gm20b.failed'].observed.split('expected:', 1)[1])
        self.assertEqual(got['gm20b.killed'].verdict, PASS)
        self.assertIn(
            'expected: killed by gpu.recover.kernel_second_timeout: PosterBoard/4', got['gm20b.killed'].observed
        )
        own = [
            record(21, 'SwitchHWTest27e', kill),
            record(22, 'SwitchHWTest27e', killed.replace('SpringBoard', 'SwitchHWTest27e')),
        ]
        results, _ = devicerun.run_checks(own, judged)
        self.assertEqual([r.verdict for r in results], [PASS, PASS])
        for line in (
            'HWTEST gpu.recover.kill_one PASS killed=1 ms=10000.0',
            'HWTEST gpu.recover.kernel_second_timeout SKIP state=1 killed=0 ms=10000.0',
        ):
            results, _ = devicerun.run_checks([*inside[::2], record(30, 'SwitchHWTest', line)], judged + windows)
            got = {r.check.name: r for r in results}
            self.assertEqual((got['gm20b.failed'].verdict, got['gm20b.killed'].verdict), (FAIL, FAIL), line)
            self.assertNotIn('expected', got['gm20b.killed'].observed)
        lines = [record(20, 'backboardd', unexplained), record(30, 'SwitchHWTest', stall)]
        results, _ = devicerun.run_checks(lines, judged + windows)
        self.assertEqual(results[0].verdict, FAIL)
        late = [record(41, 'SpringBoard', killed), record(42, 'PosterBoard', kill)]
        results, _ = devicerun.run_checks([record(30, 'SwitchHWTest', stall), *late], judged + windows)
        self.assertEqual(
            {r.check.name: r.verdict for r in results if r.check in judged},
            {'gm20b.failed': FAIL, 'gm20b.killed': FAIL},
        )
        other = [record(20, 'SpringBoard', killed, 'B00T2'), record(30, 'SwitchHWTest', stall)]
        results, _ = devicerun.run_checks(other, judged + windows)
        self.assertEqual(results[0].verdict, FAIL)

    def test_wifi_join_boot16(self):
        events = devicerun.read_ndjson(str(TEST / 'wifi.ndjson'))
        results, _ = devicerun.run_checks(events, checks.GROUPS['wifi'])
        got = {r.check.name: r for r in results}
        self.assertEqual(got['wifi.joined'].verdict, PASS)
        self.assertEqual(got['wifi.join-completion-failed'].verdict, FAIL)
        self.assertIn('-3902', got['wifi.join-completion-failed'].observed)
        self.assertNotIn('-100', got['wifi.join-completion-failed'].observed)
        self.assertEqual(got['wifi.join-report-unset'].verdict, FAIL)
        self.assertEqual(got['wifi.event-not-posted'].verdict, PASS)
        self.assertEqual(got['wifi.dhcp-link-local'].verdict, FAIL)
        self.assertEqual(got['wifi.ipv6-solicit-failed'].verdict, FAIL)

    def test_wifi_boot18_never_transmits(self):
        got = self.wifi(list(devicerun.read_ndjson(str(TEST / 'wifi_boot18.ndjson'))))
        self.assertEqual(got['wifi.joined'].verdict, PASS)
        self.assertEqual(got['wifi.en0-up'].verdict, FAIL)
        self.assertEqual(got['wifi.en0-output'].verdict, FAIL)
        self.assertIn('max(batches) 0', got['wifi.en0-output'].observed)
        self.assertEqual(got['wifi.tx-frames'].verdict, FAIL)
        self.assertEqual(got['wifi.dhcp-bound'].verdict, FAIL)
        self.assertEqual(got['wifi.dhcp-link-local'].verdict, FAIL)
        self.assertEqual(got['wifi.connection-id-refused'].verdict, FAIL)

    def test_wifi_en0_sending_and_a_lease_pass(self):
        sent = {
            'SwitchWLAN: stats ': ('tx 0 (0 KB)', 'tx 41 (6 KB)'),
            'SwitchWLAN: data ': ('en0 0 starts 0 batches', 'en0 9 starts 9 batches'),
        }
        records = []
        for record in devicerun.read_ndjson(str(TEST / 'wifi_boot18.ndjson')):
            message = record['eventMessage']
            if any(gone in message for gone in ('LINKLOCAL', 'no server', 'connection ID', 'CONNECTION_ID')):
                continue
            for prefix, (was, now) in sent.items():
                if message.startswith(prefix):
                    message = message.replace(was, now)
            records.append({**record, 'eventMessage': message})
        records.append({**records[-1], 'processImagePath': '/usr/libexec/configd', 'eventMessage': 'DHCP en0: BOUND'})
        records.insert(0, {**records[0], 'eventMessage': 'SwitchWLAN: en0 up, its output thread started'})
        got = self.wifi(records)
        self.assertEqual(got['wifi.en0-up'].verdict, PASS)
        self.assertEqual(got['wifi.en0-output'].verdict, PASS)
        self.assertEqual(got['wifi.tx-frames'].verdict, PASS)
        self.assertEqual(got['wifi.dhcp-bound'].verdict, PASS)
        self.assertEqual(got['wifi.dhcp-link-local'].verdict, PASS)
        self.assertEqual(got['wifi.connection-id-refused'].verdict, PASS)

    @staticmethod
    def wifi(records: list[dict]) -> dict[str, rules.Result]:
        results, _ = devicerun.run_checks(records, checks.GROUPS['wifi'])
        return {r.check.name: r for r in results}

    def test_wifi_checks_wait_for_a_join(self):
        got = run(*checks.GROUPS['wifi'])
        self.assertEqual(got['wifi.joined'].verdict, NO_DATA)
        self.assertEqual(got['wifi.join-completion-failed'].verdict, NO_DATA)
        self.assertEqual(got['wifi.dhcp-link-local'].verdict, NO_DATA)
        self.assertEqual(got['wifi.en0-output'].verdict, NO_DATA)
        self.assertEqual(got['wifi.dhcp-bound'].verdict, NO_DATA)


class Widgets(unittest.TestCase):
    """The display-query, window-server and widget-watchdog checks over boot 2's (22 Nov) lines."""

    @staticmethod
    def judge(name: str, log: list[dict]) -> rules.Result:
        results, _ = devicerun.run_checks(log, [c for c in checks.GROUPS['boot'] if c.name == name])
        return results[0]

    @staticmethod
    def record(path: str, subsystem: str, message: str) -> dict:
        return {
            'timestamp': '2026-11-22 23:51:55.850000+0000',
            'bootUUID': '3349B3E8',
            'processImagePath': path,
            'subsystem': subsystem,
            'category': '',
            'messageType': 'Error',
            'eventMessage': message,
        }

    def test_a_refused_display_query_fails_the_boot_in_any_process(self):
        refused = 'CoreAnimation: Unable to query displays from server (-81182718)'
        chronod = self.record('/System/Library/PrivateFrameworks/ChronoCore.framework/Support/chronod', '', refused)
        contextstored = self.record('/usr/libexec/contextstored', 'com.apple.coreanimation', refused)
        self.assertEqual(self.judge('boot.display-query-refused', []).verdict, PASS)
        result = self.judge('boot.display-query-refused', [chronod, contextstored])
        self.assertEqual(result.verdict, FAIL)
        self.assertIn('chronod x1', result.observed)
        self.assertIn('contextstored x1', result.observed)

    def test_the_window_server_is_ready_inside_a_display_querys_five_seconds(self):
        def ready(seconds: str) -> dict:
            return self.record(
                '/usr/libexec/backboardd',
                'com.apple.BackBoard',
                f'      window server ready in {seconds}s. overall 35.0843s',
            )

        self.assertEqual(self.judge('boot.window-server-ready', []).verdict, NO_DATA)
        self.assertEqual(self.judge('boot.window-server-ready', [ready('0.4127')]).verdict, PASS)
        result = self.judge('boot.window-server-ready', [ready('0.4127'), ready('21.7462')])
        self.assertEqual(result.verdict, FAIL)
        self.assertIn('max(s) 21.75', result.observed)

    def test_a_widget_session_the_watchdog_ends_fails_once_per_kill(self):
        chronod = '/System/Library/PrivateFrameworks/ChronoCore.framework/Support/chronod'
        error = (
            'Error Domain=CHSErrorDomain Code=1001 "Watchdog provision violated for getTimelines(1)" '
            'UserInfo={NSDebugDescription=Watchdog provision violated for getTimelines(1)}'
        )
        killed = [
            self.record(
                chronod,
                'com.apple.chrono',
                f'[Music.MusicWidgets-23375BEB9354] Session operation: `getTimelines(1)` error result: {error}',
            ),
            self.record(
                chronod,
                'com.apple.chrono',
                '[com.apple.Music::com.apple.Music.MusicWidgets:Music.RecentlyPlayedWidget:systemMedium::305.50/'
                f'141.00/23.97:(null)~(null)] on local reload: failed with error {error}',
            ),
        ]
        self.assertEqual(self.judge('widgets.timeline-watchdog', []).verdict, PASS)
        result = self.judge('widgets.timeline-watchdog', killed)
        self.assertEqual(result.verdict, FAIL)
        self.assertEqual(len(result.matches), 1)
        self.assertIn('Music.MusicWidgets x1', result.observed)


class Windows(unittest.TestCase):
    def test_unless_within_excuses_only_nearby_lines(self):
        restore = Check('restore', 'o', 'restore', 'timeline')
        exception = Check('exception', 'o', 'exception', unless='restore', unless_within=5.0)
        near = judged([record(10, 'restore'), record(12, 'exception')], restore, exception)
        self.assertEqual(near['exception'].verdict, PASS)
        far = judged([record(10, 'restore'), record(30, 'exception')], restore, exception)
        self.assertEqual(far['exception'].verdict, FAIL)

    def test_after_keeps_lines_soon_after_its_check(self):
        role = Check('role', 'o', 'role', 'timeline')
        halted = Check('halted', 'o', 'halted', after='role', within=2.0)
        self.assertEqual(judged([record(1, 'role'), record(2, 'halted')], role, halted)['halted'].verdict, FAIL)
        self.assertEqual(judged([record(1, 'role'), record(9, 'halted')], role, halted)['halted'].verdict, PASS)

    def test_every_needed_check_must_have_matched(self):
        a, b = Check('a', 'o', 'a', 'timeline'), Check('b', 'o', 'b', 'timeline')
        needy = Check('needy', 'o', 'needy', 'count > 0', needs=('a', 'b'))
        self.assertEqual(judged([record(1, 'a')], a, b, needy)['needy'].observed, 'no b line')
        self.assertEqual(judged([record(1, 'a'), record(2, 'b')], a, b, needy)['needy'].verdict, FAIL)

    def test_a_window_the_log_end_cuts_is_pending_and_failure_text_leads(self):
        done = Check('done', 'o', 'done', 'timeline')
        wait = Check('wait', 'o', 'wait', followed='done', within=120.0, pending_at_end=True, failure='nothing came')
        self.assertEqual(judged([record(1, 'wait'), record(20, 'tail')], done, wait)['wait'].verdict, NO_DATA)
        later = [record(1, 'wait'), {**record(0, 'tail'), 'timestamp': '2026-11-14 09:05:00.000000+0000'}]
        got = judged(later, done, wait)['wait']
        self.assertEqual(got.verdict, FAIL)
        self.assertTrue(got.observed.startswith('nothing came: 1 lines, 1 without a done line'))


class Sbim(unittest.TestCase):
    CHECK = next(c for c in checks.GROUPS['display'] if c.name == 'display.sbim')

    @staticmethod
    def line(code: str) -> dict:
        return {
            'timestamp': '2026-11-08 19:11:01.701000-0800',
            'bootUUID': '31AB01C1',
            'processImagePath': '/usr/libexec/backboardd',
            'subsystem': '',
            'category': '',
            'messageType': 'Error',
            'eventMessage': f'libEDR - EDRServerEnableSBIM: IORegistryEntrySetCFProperty() => {code}',
        }

    def verdict(self, *codes: str) -> str:
        results, _ = devicerun.run_checks([self.line(c) for c in codes], [self.CHECK])
        return results[0].verdict

    def test_boot_15s_bad_argument_fails(self):
        self.assertEqual(self.verdict('0xe00002c2', '0xe00002c2'), FAIL)

    def test_the_one_unsupported_answer_passes(self):
        self.assertEqual(self.verdict('0xe00002c7'), PASS)
        self.assertEqual(self.verdict(), PASS)


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
        self.assertEqual(crash['crash.locationd'].verdict, FAIL)
        self.assertEqual(crash['crash.spotlightknowledged'].verdict, PASS)
        self.assertEqual(crash['crash.stale-sync-waiter'].verdict, FAIL)
        self.assertEqual(crash['crash.stale-sync-waiter'].observed, '2, newest locationd-2026-11-09-034109.ips')
        self.assertEqual(devicerun.crash_rows(None)[0].verdict, NO_DATA)

    def test_the_usb_boot_prints_its_own_steps(self):
        steps = devicerun.checklist('U')
        self.assertIn('1 A USB stick', steps)
        self.assertIn('kernel HID after 120 s, up to 3 tries', steps)
        self.assertNotIn('App Library scroll', steps)

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

    def test_boot_prefetch_lead_parses_line(self):
        lead_check = next(c for c in checks.GROUPS['boot'] if c.name == 'boot.prefetch-lead')
        summary_check = next(c for c in checks.GROUPS['boot'] if c.name == 'boot.prefetch')
        lead_msg = (
            'SwitchBootPrefetch: 120 MiB paged in ahead (110 used, 2 reclaimed unused, '
            'lead 8 of bound 64), 45 MiB of demand reads by volume (15/20/10), '
            'card ms 1400/900, requests > 64 KiB replay 200/240, demand 12/80'
        )
        saved_msg = (
            'SwitchBootPrefetch: list saved: 42 runs, 12 MiB '
            '(4/3/5 MiB cx/root/Data; 10 kept of the last list, 1 dropped unused, 0 evicted) in 35 ms'
        )
        records = [
            {
                'timestamp': '2026-11-23 10:00:10.000000-0800',
                'bootUUID': '31AB01C1',
                'processImagePath': '/kernel',
                'subsystem': '',
                'category': '',
                'messageType': 'Default',
                'eventMessage': msg,
            }
            for msg in (lead_msg, saved_msg)
        ]
        results, _ = devicerun.run_checks(records, [summary_check, lead_check])
        by_name = {r.check.name: r for r in results}
        self.assertEqual(by_name['boot.prefetch-lead'].verdict, INFO)
        self.assertIn('120/110/2/8/64/45/15/20/10/1400/900/200/240/12/80', by_name['boot.prefetch-lead'].observed)
        self.assertEqual(by_name['boot.prefetch'].verdict, INFO)
        self.assertEqual(len(by_name['boot.prefetch'].matches), 1)
        self.assertIn('list saved:', by_name['boot.prefetch'].matches[0]['message'])

    def test_launchd_prngseedd_fails_on_boot_2_respawn_and_passes_when_absent(self):
        check = next(c for c in checks.GROUPS['launchd'] if c.name == 'launchd.prngseedd')
        spawned = {
            'timestamp': '2026-11-22 23:56:53.760000+0000',
            'bootUUID': 'B2',
            'processImagePath': '/sbin/launchd',
            'subsystem': 'user/501/com.apple.prngseedd [492]',
            'category': '',
            'messageType': 'Default',
            'eventMessage': 'exited due to exit(0), ran for 1030ms',
        }
        failed, _ = devicerun.run_checks([spawned], [check])
        self.assertEqual(failed[0].verdict, FAIL)
        clean, _ = devicerun.run_checks([], [check])
        self.assertEqual(clean[0].verdict, PASS)


if __name__ == '__main__':
    unittest.main()
