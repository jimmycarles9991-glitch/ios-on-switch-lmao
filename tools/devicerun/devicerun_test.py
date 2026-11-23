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
                self.assertTrue(not c.during or c.during in names, c.name)
                self.assertTrue(not c.per or c.per in names, c.name)
                self.assertTrue(not c.preceded or c.preceded in names, c.name)

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


class UsbHost(unittest.TestCase):
    """The host-mode checks over boot-1109-2145's lines (a keyboard plugged twice) and a boot that enumerates."""

    @staticmethod
    def usb(records: list[dict]) -> dict[str, rules.Result]:
        results, _ = devicerun.run_checks(records, checks.GROUPS['usb'])
        return {r.check.name: r for r in results}

    @staticmethod
    def kernel(second: int, message: str, boot: str = 'B0000000-0000-0000-0000-000000000001') -> dict:
        return {
            'timestamp': f'2026-11-10 09:00:{second:02d}.000000+0000',
            'bootUUID': boot,
            'processImagePath': '/kernel.development',
            'subsystem': '',
            'category': '',
            'messageType': 'Default',
            'eventMessage': message,
        }

    def test_boot_1109_2145(self):
        got = self.usb(list(devicerun.read_ndjson(str(TEST / 'usb_host_boot1109.ndjson'))))
        for name in (
            'usb.host-save-state',
            'usb.host-exception',
            'usb.host-port-properties',
            'usb.host-connect',
            'usb.host-device-arrives',
        ):
            self.assertEqual(got[name].verdict, FAIL, name)
        self.assertIn('save state failed', got['usb.host-exception'].observed)

    def test_a_keyboard_that_enumerates(self):
        got = self.usb(
            [
                self.kernel(0, 'SwitchUSB: host controller up (AppleUSBXHCI, 1 interrupter, 3 shown to AppleUSBXHCI)'),
                self.kernel(1, 'SwitchUSB: host role (a device on the port, VBUS on)'),
                self.kernel(
                    2,
                    'SwitchUSB: host role +1 s (change 1, 1/2): xHCI USBCMD 0x801 USBSTS 0x8, pad 0 = port 5 '
                    '(USB2 5+5): U0 low speed, SS port 0 = port 1 (USB3 1+4): RxDetect nothing',
                ),
                self.kernel(3, 'SwitchUSB: USB device Keyboard (05ac:024f) registered at location 0x5000000'),
            ]
        )
        for name in (
            'usb.host-save-state',
            'usb.host-exception',
            'usb.host-port-properties',
            'usb.host-connect',
            'usb.host-device-arrives',
        ):
            self.assertEqual(got[name].verdict, PASS, name)

    def test_the_restore_after_sc7_excuses_its_exception(self):
        exception = (
            'SwitchXHCI@00000000: AppleUSBHostController::hardwareExceptionThreadCallGated: 0x00000004 '
            '(save state failed)'
        )
        up = 'SwitchUSB: host controller up (AppleUSBXHCI, 1 interrupter, 3 shown to AppleUSBXHCI)'
        restore = "SwitchUSB: XUSB host restore after SC7: the controller's state went with the falcon, USBSTS.SRE"
        got = self.usb([self.kernel(0, up), self.kernel(1, restore), self.kernel(2, exception)])
        self.assertEqual(got['usb.host-exception'].verdict, PASS)
        got = self.usb([self.kernel(0, up), self.kernel(2, exception)])
        self.assertEqual(got['usb.host-exception'].verdict, FAIL)


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
