"""tools/perf over lines as boot 2 of 22 Nov logged them (iOS 27, a debug kernel), cut to what each test needs."""

import gzip
import json
import os
import struct
import tempfile
import unittest
import zlib
from datetime import UTC, datetime, timedelta
from pathlib import Path

from tools.perf import cpu, dumps, gpu, ips, pacing, perf, recfile, report, session, source, symbols, vm, windows, work
from tools.perf.source import TICK_HZ, Rec

T0 = datetime(2026, 11, 23, 0, 13, 20, tzinfo=UTC)
MACH0 = 25_640_000_000
PERIOD_TICKS = int(TICK_HZ / 60)

# A present two periods after the one before, its first submission 22.5 ms after the vsync it started from.
LATE_RECORD = (
    'SwitchFrame: latch {latch} after 2 frames (33331 us), cause ca; origin vsync {origin}; first submit 22523, '
    'last submit 22523 (1, exec 20 us), swap 22763, offer 22895, gpu done 24133, flip 24047, latch 33331 us; other '
    'submits 0; swap thread base 54 sched 54 mode 2 qos 0; cpu 1785 MHz at vsync 1785 at swap; imp 0 0 us, bind 0 '
    '0 us, alloc 0 0 us, free 0, clean 0 KiB 0 us'
)
# One whose own chain latched a period after its origin: the frame before it never presented.
GAP = (
    'SwitchFrame: latch {latch} after 2 frames (33333 us), cause ca; origin vsync {origin}; first submit 8700, '
    'last submit 8700 (1, exec 20 us), swap 9000, offer 9100, gpu done 10900, flip 10900, latch 16700 us; other '
    'submits 0; swap thread base 54 sched 54 mode 2 qos 0; cpu 1785 MHz at vsync 1785 at swap'
)
QUEUED = (
    'SwitchFrame: latch {latch} after 13 frames (216700 us), cause queue; origin vsync {origin}; first submit '
    '2700, last submit 2700 (1, exec 20 us), swap 2800, offer 215900, gpu done -1, flip 215900, latch 216700 us; '
    'other submits 0; swap thread base 54 sched 54 mode 2 qos 0; cpu 1785 MHz at vsync 1785 at swap'
)
FROM_SWAP = (
    'SwitchFrame: latch {latch} after 3 frames (50005 us), cause ca; origin swap {origin}; first submit -204, last '
    'submit -204 (1, exec 16 us), swap 0, offer 106, gpu done -1, flip 127, latch 12633 us; other submits 0; swap '
    'thread base 54 sched 54 mode 2 qos 0; cpu 1785 MHz at vsync 1785 at swap'
)
# The render thread's line for a vsync: its swap, how it spent the time to it, and its longest block.
RENDER = (
    'SwitchFrameRender: vsync {vsync}: woke 61 us after it; first submit none; swap {swap} us (cpu {cpu}, blocked '
    '{blocked} us in 3, waiting for a core 34 us); longest block {longest} us, user pc dsc 0x23c991cd4 lr dsc '
    '0x23c995308, kernel continuation 0x0; dropped 0'
)
USER = (
    'SwitchFrameUser: backboardd cb 27305 commit {commit} enc {enc} {enc2} replay 0 0 ret 0 owait 0 0 fence {done} '
    'done {done} back 0 slot 0 0 prev 0 dropped 0 surf 1 imp 1 hit 0 made 0'
)
STATS = (
    'SwitchDisplay stats: 5016 ms: 262 DC A interrupts, 263 swaps 52.4/s; present 262 (262 timed): interval p50 17 '
    'p99 50 p99.9 50 max 50.0 ms, frames 1:239 2:7 3:16 4+:0, >1.5x 23 >2x 16, gaps 0/0; swap-to-scan p50 9'
)
POWER = (
    'SwitchPower: 10005 ms: cpu0 c7 0.0% (0, 0 skipped, 0 frame wakes) wfi 32.2% busy 67.8%, cpu1 c7 0.0% (0, 0 '
    'skipped, 0 frame wakes) wfi 37.2% busy 62.8%; c7 timer exits 0'
)
# probes-kernel's per-window CPU lines, as tools/devicerun/cpu_test.py writes them
CORES = (
    "SwitchSched cores (10000 ms; ms: a core's threads, its idle thread split; 310 us to take): cpu0 threads 4000 "
    '(irq 300), idle 6000 = asleep 5700 + irq 150 + rest 150, 12000 sleeps; cpu1 threads 1000 (irq 10), idle 9000 = '
    'asleep 8800 + irq 100 + rest 100, 8000 sleeps; a64x traps 0, unhandled 0, text pages 12'
)
TASKS = (
    'SwitchSched tasks (10000 ms; ms user+system, top 8 by both): SpringBoard[393] 1200+300, backboardd[69] 500+200; '
    'all 312 processes 3900+1800, exited 2 (410+30, most prngseedd[1234] 80+2), untracked 0'
)
MADE = (
    'SwitchMetal: a state the pack lacks made in 287 ms (DF, by the shader compiler; queue 3136 us, compile 283510 '
    'us, answer 154 us at priority 31, wake 173 us, store 225 us) after 0 ms for a compile slot: a4fe2ccba8a2dd5d'
)
HANG = (
    '__##__signpost.description#____#begin_time#_##_#{begin}##__##, '
    '__##__signpost.description#____#end_time#_##_#{end}##__##, '
    'BundleIdOverride=__##__signpost.description#____#attribute#_##_#com.apple.springboard##__## '
    'type=__##__signpost.telemetry#____#string2#_##_#Extended Launch Runloop Hang##__## enableTelemetry=YES'
)


def rec(msg: str, at: float, proc: str = 'kernel', **kw: object) -> Rec:
    """Return a record at `at` seconds after T0, its mach time on the same clock."""
    fields = {'boot': 'B', 'pid': 0, 'tid': 1, 'subsystem': '', 'category': '', 'name': '', 'kind': '', 'ident': 0}
    fields.update(kw)
    return Rec(T0 + timedelta(seconds=at), MACH0 + int(at * TICK_HZ), proc=proc, msg=msg, **fields)


def frame(template: str, at: float, origin_ticks: int, **kw: int) -> Rec:
    return rec(template.format(latch=origin_ticks + 2 * PERIOD_TICKS, origin=origin_ticks, **kw), at)


class GpuTest(unittest.TestCase):
    def test_the_smmu_line_gives_each_windows_hit_rates_and_retirement_invalidates(self):
        recs = [
            rec(
                'tegra-gm20b: smmu counters on from here (TLB_CONFIG 0xb0000030 PTC_CONFIG 0xa8000f3f); retirement L2 '
                'invalidates 0 for 0 objects, 0 us CPU',
                1.0,
            ),
            rec(
                'tegra-gm20b: smmu tlb 900 hits 100 misses (90.0% hit), ptc 95 hits 5 misses (95.0% hit); '
                'retirement L2 invalidates 12 for 30 objects, 2400 us CPU',
                6.0,
            ),
        ]
        g = gpu.read(recs)
        self.assertEqual(g.smmu, [(900, 100, 95, 5)])
        self.assertEqual(g.retire, [(0, 0), (12, 2400)])
        text = '\n'.join(gpu.lines(g))
        self.assertIn('SMMU TLB hits per stats window: p50 90.0%, worst 90.0%; 100 misses over 1 windows', text)
        self.assertIn('retirement L2 invalidates per stats window: p50 12, max 12; 2.4 ms of CPU over 2 windows', text)


class PacingTest(unittest.TestCase):
    def test_the_stage_is_the_first_point_past_the_frame_end(self):
        origins = [MACH0 + 1000 + i * 60 * PERIOD_TICKS for i in range(6)]
        lines = (
            (22129, 4535, 17497, 16607),  # asleep in one block for a whole period: waiting, not recording
            (22700, 15000, 7600, 1500),  # on a core most of the way
            (8474, 2290, 6117, 905),  # another swap than the one the display took: not this present's line
        )
        recs = [
            # the render thread's stand-in vsync is 40 us after the frame end
            rec(RENDER.format(vsync=origin + 768, swap=s, cpu=c, blocked=b, longest=lng), i * 1.0)
            for i, (origin, (s, c, b, lng)) in enumerate(zip(origins, lines, strict=False))
        ]
        recs += [frame(LATE_RECORD, i + 0.04, origins[i]) for i in range(3)]
        recs += [frame(t, i + 3.04, origins[i + 3]) for i, t in enumerate((GAP, QUEUED, FROM_SWAP))]
        recs.sort(key=lambda r: r.time)
        stages = [s.stage for s in pacing.slow_presents(recs)]
        expected = ['render thread waited', 'record, on core', 'record', 'gap before it', 'display queue', 'unanchored']
        self.assertEqual(stages, expected)

    def test_a_first_submit_before_the_vsync_or_none_still_places_the_frame(self):
        """Lines of boot 2693043C (25 Nov), which the classifier used to call unclassified."""
        before = (
            'SwitchFrame: latch {latch} after 3 frames (49999 us), cause ca; origin vsync {origin}; first submit '
            '-24689, last submit 8756 (2, exec 35 us), swap 8845, offer 8971, gpu done 17602, flip 17604, latch 33332 '
            'us; other submits 2; swap thread base 54 sched 54 mode 2 qos 0; cpu 1785 MHz at vsync 1785 at swap; imp 2 '
            '211 us, bind 2 45 us, alloc 0 0 us, free 2, clean 32 KiB 12 us; gpu busy 33.5%'
        )
        none = (
            'SwitchFrame: latch {latch} after 2 frames (33315 us), cause ca; origin vsync {origin}; first submit -1, '
            'last submit -1 (0, exec 0 us), swap 18375, offer 18437, gpu done -1, flip 18472, latch 33314 us; other '
            'submits 0; swap thread base 54 sched 54 mode 2 qos 0; cpu 1785 MHz at vsync 1785 at swap; imp 0 0 us, '
            'bind 0 0 us, alloc 0 0 us, free 0, clean 0 KiB 0 us; gpu busy 0.0%'
        )
        slows = pacing.slow_presents([frame(before, 0.04, MACH0 + 1000), frame(none, 1.04, MACH0 + 60 * PERIOD_TICKS)])
        self.assertEqual([s.stage for s in slows], ['gpu', 'encode'])
        self.assertEqual([(s.chain_periods, s.frame.frames - 1) for s in slows], [(1, 2), (1, 1)])
        self.assertIn('lost in: gpu (1 in its chain, 1 before its vsync)', pacing.timeline(slows[0])[0])

    def test_the_render_threads_window_on_the_frame_line_places_its_wake_and_recording(self):
        origin = MACH0 + 1000
        window = '; render vsync {vsync} woke {woke} first {first}'
        late_first = LATE_RECORD.replace('first submit 22523', 'first submit -3000') + window
        recs = [
            frame(late_first, 0.04, origin, vsync=origin + 768, woke=150, first=21000),
            frame(
                LATE_RECORD + window,
                1.04,
                origin + 60 * PERIOD_TICKS,
                vsync=origin + 60 * PERIOD_TICKS,
                woke=18000,
                first=-1,
            ),
            frame(LATE_RECORD + window, 2.04, origin + 120 * PERIOD_TICKS, vsync=origin, woke=18000, first=-1),
        ]
        slows = pacing.slow_presents(recs)
        self.assertEqual([s.stage for s in slows], ['record', 'late wake', 'record'])
        self.assertEqual([s.window for s in slows], [(150, 21000), (18000, -1), None])
        self.assertIn('render thread window: woke +0.1 ms, first submit +21.0 ms', '\n'.join(pacing.timeline(slows[0])))

    def test_the_commits_before_the_swap_join_the_present(self):
        origin = MACH0 + 1000
        commit = origin + int(0.020 * TICK_HZ)
        user = USER.format(commit=commit, enc=commit + 100, enc2=commit + 200, done=commit + 900)
        recs = [rec(user, 0.02, 'backboardd'), frame(LATE_RECORD, 0.04, origin)]
        slow = pacing.slow_presents(recs)[0]
        self.assertEqual([(p, cb) for p, cb, _ in slow.users], [('backboardd', 27305)])
        self.assertIn('backboardd cb 27305: commit +20.0 ms', '\n'.join(pacing.timeline(slow)))

    def test_a_cycle_is_named_by_its_longest_period(self):
        grid = int(0.5 * TICK_HZ)
        recs = [
            rec(LATE_RECORD.format(latch=MACH0 + i * grid + 3 * PERIOD_TICKS, origin=MACH0 + i * grid), i * 0.5)
            for i in range(30)
        ]
        self.assertIn('30 of 30 late presents keep to a 500 ms cycle', pacing.periodic(pacing.slow_presents(recs)))

    def test_a_short_window_takes_the_stats_windows_around_it(self):
        stats = pacing.stats_windows([rec(STATS, 5.016), rec(STATS, 10.032)])
        inside = windows.Window('test', 'pacing.list', T0, T0 + timedelta(seconds=10.5))
        brief = windows.Window('touch', 'touch', T0 + timedelta(seconds=6), T0 + timedelta(seconds=7))
        self.assertEqual(pacing.distribution(inside, stats), ([478, 14, 32, 0], 10032, 50.0, False))
        self.assertEqual(pacing.distribution(brief, stats)[0], [239, 7, 16, 0])
        self.assertTrue(pacing.distribution(brief, stats)[3])


# kernel/tests/xnu_recorder_test.cpp's TheHeaderIsTheLineToolsPerfReads writes this same line.
REC_HEADER = (
    'SwitchRecorder 2 boot=2693043C-01C2-4E03-9DA5-FDBCF1DD9490 kernel=724459E3-00EE-E993-E2A1-D165FC9289'
    '3A seq=3 ask=41 reason=hang tid=514 value=320 at=4340000000 shot_at=4340057600 frozen_at=4340096000 '
    'done_at=4340672000 shot_us=3000 ring_us=30000 tod=1795548000.250000 shot=19536 ring=23552 deflate=90'
    '00 ring_lost=0'
    ' on_ms=20 blocked_ms=145 runnable_ms=98'
)
TESTDATA = Path(__file__).resolve().parent / 'testdata'


def rec_file(folder: Path, header: str, shot: bytes, ring: bytes, cut: int = 0) -> Path:
    """Write a dump as store.c does: the stackshot and the ring as one raw deflate stream after its text header."""
    packer = zlib.compressobj(1, zlib.DEFLATED, -zlib.MAX_WBITS)
    body = packer.compress(shot + ring) + packer.flush()
    fields = dict(f.split('=', 1) for f in header.split()[2:])
    fields.update(shot=str(len(shot)), ring=str(len(ring)), deflate=str(len(body)))
    text = 'SwitchRecorder 2 ' + ' '.join(f'{k}={v}' for k, v in fields.items())
    path = folder / 'Switch-recorder-0.rec'
    path.write_bytes((text.ljust(recfile.HEADER - 1) + '\n').encode() + body[: len(body) - cut])
    return path


class RecFileTest(unittest.TestCase):
    def test_the_kernels_header_reads_back(self):
        h = recfile.fields(REC_HEADER)
        self.assertEqual(
            (h['boot'][:8], h['seq'], h['reason'], h['tid'], h['ring_lost']), ('2693043C', '3', 'hang', '514', '0')
        )
        d = recfile.Dump('x', h)
        self.assertAlmostEqual(d.after_trigger_ms('shot_at'), 3.0)
        self.assertAlmostEqual(d.after_trigger_ms('done_at'), 35.0)
        self.assertIn('stackshot +3.0 ms, ring frozen +5.0 ms, both in memory +35.0 ms', d.describe())
        self.assertIn('its stretch: on a core 20 ms, blocked 145 ms, waiting for a core 98 ms', d.describe())

    def test_a_dump_file_inflates_to_its_stackshot_and_ring_and_a_partial_one_says_so(self):
        shot = (TESTDATA / 'Switch-recorder-3.kcdata').read_bytes()
        ring = (TESTDATA / 'Switch-recorder-3.ktrace').read_bytes()
        with tempfile.TemporaryDirectory() as d:
            path = rec_file(Path(d), REC_HEADER, shot, ring)
            dump = recfile.read(str(path))
            self.assertEqual((dump.problem, dump.shot, dump.ring), ('', shot, ring))
            self.assertEqual(dumps.find(d), [(str(path), str(path))])
            self.assertEqual(
                recfile.read(str(rec_file(Path(d), REC_HEADER, shot, ring, cut=100))).problem[:12], 'the body is '
            )
            path.write_bytes(bytes(recfile.HEADER) + b'x')
            self.assertTrue(recfile.read(str(path)).problem.startswith('no header'))

    def test_the_recorders_lines_add_up(self):
        capture = (
            'SwitchRecorder: capture {seq} ({reason} ask 9, thread 514, value 320) at 4340000000: stackshot 1007 KiB '
            'in 3000 us (+3000 us), ring 17969 KiB in 30000 us (+5000 us), in memory +{done} us{lost}'
        )
        recs = [
            rec(capture.format(seq=1, reason='hang', done=35000, lost=''), 1.0),
            rec(capture.format(seq=2, reason='mark', done=41000, lost='; no ring: no memory for it'), 2.0),
            rec('SwitchRecorder: refused hang ask 12 (thread 606, value 9283) at 4440000000: gap', 3.0),
            rec(
                'SwitchRecorder: dump 1 (hang ask 9, thread 514, value 320) in slot 4: stackshot 1007 KiB and ring '
                '17969 KiB as 3700 KiB, deflate 400000 us, write 90000 us, 2100 ms after its trigger',
                4.0,
            ),
        ]
        text = '\n'.join(recfile.recorder_lines(recs))
        self.assertIn('captures: 2 (hang 1, mark 1); in memory p50 +41.0 ms, max +41.0 ms after the trigger', text)
        self.assertIn('captures without a ring: no memory for it 1', text)
        self.assertIn('refused (one line a second per reason and thread): hang gap 1', text)
        self.assertIn('written: 1, 2.1 s after their trigger at the median', text)


class WindowsTest(unittest.TestCase):
    def test_a_recorder_builds_kprof_says_it_sampled_the_boot_window_alone(self):
        recs = [
            rec('SwitchKprof: sampling every 4 ms of CPU work, 9000 kernel functions, summary every 10 s', 1.0),
            rec('SwitchKprof: boot window over: off after 240 s of uptime (a recorder build)', 240.0),
        ]
        state, _ = cpu.kprof(recs)
        self.assertTrue(state.startswith('the boot window only, until'), state)

    def test_a_press_is_springboards_line_with_the_recorders_answer(self):
        sb = {'subsystem': 'com.apple.SpringBoard', 'category': 'HIDKeyboardEvents'}
        recs = [
            rec('Snapshot button pressed down', 10.0, 'SpringBoard', **sb),
            rec(
                'SwitchRecorder: capture 4 (mark ask 30, thread 0, value 0) at 1: stackshot 1 KiB in 1 us (+1 us), '
                'ring 1 KiB in 1 us (+1 us), in memory +1 us',
                10.4,
            ),
            rec('Snapshot button pressed down', 20.0, 'SpringBoard', **sb),
            rec('SwitchRecorder: refused mark ask 31 (thread 0, value 0) at 2: full', 20.1),
            rec('Snapshot button pressed down', 30.0, 'SpringBoard', **sb),
            rec(
                'SwitchRecorder: capture 5 (mark ask 32, thread 0, value 0) at 3: stackshot 1 KiB in 1 us (+1 us), '
                'ring 1 KiB in 1 us (+1 us), in memory +1 us',
                40.0,
            ),
            rec(
                'SwitchMark: ui.title_scroll.back.1 500 ms',
                41.0,
                'SwitchHWTest27',
                subsystem='com.brunostjohn.switch.hwtest',
            ),
        ]
        got = [(w.name, w.note, w.end) for w in windows.presses(recs)]
        self.assertEqual(
            got,
            [
                ('capture', 'capture 4', recs[0].time),
                ('capture', 'refused (full)', recs[2].time),
                ('capture', 'no recorder line', recs[4].time),
                ('mark', 'capture 5', recs[5].time),
            ],
        )
        self.assertEqual([w.name for w in windows.marks(recs)], ['ui.title_scroll.back.1'])

    def test_label_images_against_the_home_screen_becoming_visible(self):
        icon = {'subsystem': 'com.apple.SpringBoard', 'category': 'Icon'}
        metal = {'subsystem': 'com.brunostjohn.switch.TegraGM20BMetal'}
        recs = [
            rec(
                'Changing content visibility of icon manager 0x79cb8b700 from hidden to visible',
                10.0,
                'SpringBoard',
                **icon,
            ),
            *[
                rec(f'SwitchLabels: icon label image {n} set at {n}', 60.0 + n, 'SpringBoard', **metal)
                for n in range(1, 25)
            ],
        ]
        (row,) = report.labels_row(session.build('B', recs))
        self.assertIn('first label image +51.0 s, the 20th +70.0 s, 24 logged', row)
        missing = [
            rec('SwitchLabels: no -[SBIconPLKLegibilityView setImage:] in this SpringBoard: no label times', 1.0)
        ]
        self.assertIn(
            'no label lines: SwitchLabels: no -[SBIconPLKLegibilityView',
            report.labels_row(session.build('B', missing))[0],
        )

    def test_a_hang_after_a_press_names_the_recorder_work_it_overlapped(self):
        sb = {'subsystem': 'com.apple.SpringBoard', 'category': 'HIDKeyboardEvents'}
        hang = (
            '__##__signpost.description#____#begin_time#_##_#{begin}##__##, '
            '__##__signpost.description#____#end_time#_##_#{end}##__##, '
            'type=__##__signpost.telemetry#____#string2#_##_#Hang##__##'
        )
        recs = [
            rec('Snapshot button pressed down', 10.0, 'SpringBoard', **sb),
            rec(
                'SwitchRecorder: capture 4 (mark ask 30, thread 0, value 0) at 1: stackshot 1 KiB in 1 us (+1 us), '
                'ring 1 KiB in 1 us (+1 us), in memory +40000 us',
                10.34,
            ),
            rec(
                hang.format(begin=MACH0 + int(10.31 * TICK_HZ), end=MACH0 + int(10.6 * TICK_HZ)),
                10.6,
                'ScreenshotServicesService',
                name='HangInterval',
            ),
            rec(
                hang.format(begin=MACH0 + int(13.0 * TICK_HZ), end=MACH0 + int(13.4 * TICK_HZ)),
                13.4,
                'SpringBoard',
                name='HangInterval',
            ),
        ]
        s = session.build('B', recs)
        text = report.hangs_after(s, s.windows['press'][0])
        self.assertIn('ScreenshotServicesService 0.29 s at +0.3 s (during capture 4)', text)
        self.assertIn('SpringBoard 0.40 s at +3.0 s (no recorder work)', text)

    def test_a_test_window_ends_at_its_line_and_lasts_its_ms(self):
        line = rec('HWTEST display.snapshot PASS frames=12 ms=17210.4', 20.0, 'SwitchHWTest', pid=560)
        (w,) = windows.tests([line])
        self.assertEqual((w.name, w.note, round(w.ms)), ('display.snapshot', 'PASS', 17210))

    def test_a_launch_runs_from_the_first_ask_to_its_content_ready(self):
        fb = {'subsystem': 'com.apple.FrontBoard'}
        ask = 'Asked to bootstrap a new process with identity: app<com.apple.Preferences(FEEDEEEE-DDDD)>'
        recs = [
            rec(ask, 0.0, 'SpringBoard', **fb),
            rec('[app<com.apple.Preferences>:659] Bootstrap success!', 0.02, 'SpringBoard', **fb),
            rec(ask, 1.1, 'SpringBoard', **fb),
            rec(
                '[(FBSceneManager):sceneID:com.apple.Preferences-default] scene content state changed: ready',
                8.93,
                'SpringBoard',
                **fb,
            ),
        ]
        (w,) = windows.launches(recs)
        self.assertEqual((w.name, w.pid, round(w.ms)), ('com.apple.Preferences', 659, 8930))

    def test_a_mark_is_the_kernels_or_hw_tests_and_spans_its_ms(self):
        recs = [
            rec('SwitchMark: capture', 2.0),
            rec(
                'SwitchMark: ui.title_scroll.back.1 500 ms frames=31',
                9.0,
                'SwitchHWTest27',
                subsystem='com.brunostjohn.switch.hwtest',
            ),
            rec('SwitchMark: forged 100 ms', 9.0, 'SpringBoard'),
        ]
        capture, scroll = windows.marks(recs)
        self.assertEqual((capture.name, capture.start, capture.end), ('capture', recs[0].time, recs[0].time))
        self.assertEqual(scroll.name, 'ui.title_scroll.back.1')
        self.assertEqual(scroll.end - scroll.start, timedelta(milliseconds=500))

    def test_a_hang_spans_its_signpost_begin_and_end(self):
        begin, end = MACH0 + int(2 * TICK_HZ), MACH0 + int(5.49 * TICK_HZ)
        sp = rec(HANG.format(begin=begin, end=end), 5.5, 'SpringBoard', pid=400, name='HangInterval', kind='event')
        (w,) = windows.hangs([sp])
        self.assertEqual((w.proc, w.name, w.note), ('SpringBoard', '3.49 s', 'Extended Launch Runloop Hang'))
        self.assertAlmostEqual((w.start - T0).total_seconds(), 2.0, places=3)


class WorkTest(unittest.TestCase):
    def test_creations_and_stall_signposts_cost_what_their_lines_say(self):
        stall = rec(
            f'__##__signpost.description#____#begin_time#_##_#{MACH0 + int(3.62 * TICK_HZ)}##__## seed=0x3',
            8.92,
            'Preferences',
            pid=659,
            subsystem='com.apple.coreanimation',
            category='Transaction.Stalls',
            name='Commit',
            kind='event',
        )
        costs = work.compiles([rec(MADE, 1.0, 'iconservicesagent')]) + work.stalls([stall])
        self.assertEqual(
            [(c.proc, c.what, round(c.ms)) for c in costs],
            [
                ('iconservicesagent', 'state made (compiler)', 287),
                ('Preferences', 'Core Animation transaction, begin to commit', 5300),
            ],
        )
        self.assertTrue(work.apple(costs[1]))


class CommitTest(unittest.TestCase):
    def test_a_launch_names_its_first_commit_of_either_category(self):
        sp = {'name': 'Commit', 'kind': 'event', 'subsystem': 'com.apple.coreanimation', 'pid': 659}
        begun = '__##__signpost.description#____#begin_time#_##_#{}##__## seed=0x3'
        recs = [
            rec(begun.format(MACH0 + int(3.62 * TICK_HZ)), 8.93, 'Preferences', category='Transaction.Stalls', **sp),
            rec(begun.format(MACH0 + int(9.0 * TICK_HZ)), 9.1, 'Preferences', category='Transaction', **sp),
        ]
        began, ended = work.first_commit(recs, 659, T0, T0 + timedelta(seconds=10))
        self.assertAlmostEqual((began - T0).total_seconds(), 3.62, places=3)
        self.assertEqual(ended, T0 + timedelta(seconds=8.93))


class ReportsTest(unittest.TestCase):
    def test_only_this_boots_reports_are_read_and_their_main_threads_print_with_their_waits(self):
        body = {
            'bootSessionUUID': 'b',
            'pid': 30,
            'uptime': 130,
            'termination': {'details': ['checkin with service: SpringBoard failed']},
            'stackshot': {
                'binaryImages': [['a2dca908', 0x180000000, 'S']],
                'processByPid': {
                    '30': {
                        'pid': 30,
                        'procname': 'SpringBoard',
                        'userTimeTask': 12.8,
                        'systemTimeTask': 6.4,
                        'waitInfo': ['thread 520: unknown type 22 (owner 0, context 0x1)'],
                        'threadById': {
                            '2568': {'state': ['TH_WAIT'], 'snapshotFlags': []},
                            '520': {'state': ['TH_WAIT'], 'snapshotFlags': ['kThreadMain'], 'userFrames': [[0, 0x10]]},
                        },
                    }
                },
            },
        }
        with tempfile.TemporaryDirectory() as d:
            for name, boot, build in (('SpringBoard-a.ips', 'B', '24B119'), ('SpringBoard-b.ips', 'OTHER', '21H461')):
                head = {'bug_type': '409', 'name': 'SpringBoard', 'timestamp': '2026-11-22 23:53:17'}
                with open(os.path.join(d, name), 'w') as f:
                    f.write(json.dumps(head | {'os_version': f'iPhone OS 27.2 ({build})'}))
                    f.write('\n' + json.dumps(body | {'bootSessionUUID': boot}))
            reports = ips.scan(d)
        text = '\n'.join(ips.summary(reports, {'B'}, {}, ('SpringBoard',)) + ips.watchdogs(reports))
        self.assertIn('1 reports of this boot; 1 of other boots in the folder (1 watchdog stackshot)', text)
        self.assertIn('SpringBoard[30] main thread 520: TH_WAIT', text)
        self.assertIn('thread 520: page busy (a page-in or page change in progress) (owner 0, context 0x1)', text)
        self.assertIn('dsc 0x180000010', text)
        self.assertIn('SpringBoard at 130 s uptime: main thread TH_WAIT, thread 520: page busy', text)
        self.assertIn('SpringBoard at 130 s uptime: main thread TH_WAIT, thread 520: unknown type 22', text)


class SessionTest(unittest.TestCase):
    def test_a_window_dossier_names_its_busy_cores_and_its_work(self):
        recs = [rec(POWER, 10.0), rec(MADE, 4.0, 'iconservicesagent'), rec(STATS, 5.016)]
        s = session.build('B', recs)
        w = windows.Window('launch', 'x', T0 + timedelta(seconds=3), T0 + timedelta(seconds=5))
        text = '\n'.join(session.dossier(s, w))
        self.assertIn('busy cores 1.31', text)
        self.assertIn('iconservicesagent: 1 x state made (compiler), 287 ms', text)
        self.assertIn('processes: not attributable', text)

    def test_a_window_names_the_processes_of_the_cpu_windows_ending_inside_it(self):
        s = session.build('B', [rec(CORES, 10.0), rec(TASKS, 10.001)])
        w = windows.Window('launch', 'x', T0 + timedelta(seconds=5), T0 + timedelta(seconds=11))
        self.assertIn('processes: SpringBoard 150 ms/s, backboardd 70 ms/s', '\n'.join(session.dossier(s, w)))

    def test_the_report_has_every_section_on_lines_of_any_kind(self):
        recs = [rec(POWER, 10.0), rec(STATS, 5.016), frame(LATE_RECORD, 1.0, MACH0 + 1000)]
        text = report.render(session.build('B', recs), report.Pulled())
        for heading in ('## Frame pacing', '## Slow presents', '## CPU', '## GPU', '## Hangs', '## Launches'):
            self.assertIn(heading, text)


class PullTest(unittest.TestCase):
    def test_a_crash_folder_alone_reports_its_dumps(self):
        testdata = Path(__file__).resolve().parent / 'testdata'
        with tempfile.TemporaryDirectory() as d:
            for name in ('Switch-recorder-3.kcdata', 'Switch-recorder-3.ktrace'):
                (Path(d) / name).write_bytes((testdata / name).read_bytes())
            out = Path(d) / 'report.txt'
            self.assertEqual(perf.main(['report', '--crashes', d, '--out', str(out)]), 0)
            text = out.read_text()
        self.assertIn('Flight recorder dumps in', text)
        self.assertIn('Switch-recorder-3.ktrace: hang', text)
        with self.assertRaises(SystemExit):
            perf.main(['report'])

    def test_a_pull_reaches_back_to_the_last_pull_and_the_report_names_a_hole(self):
        with tempfile.TemporaryDirectory() as d:
            now = datetime(2026, 11, 25, 20, 13, 23, tzinfo=UTC)
            self.assertEqual(perf.pull_age(Path(d), 900, now), 900)
            (Path(d) / '20261125-194944').mkdir()
            (Path(d) / '1125-2005-hitches').mkdir()
            self.assertEqual(perf.pull_age(Path(d), 900, now), 1419 + perf.PULL_OVERLAP_S)
            self.assertEqual(perf.pull_age(Path(d), 7200, now), 7200)
        recs = [rec('SwitchSched cores (10000 ms)', 0.0), rec('SwitchSched cores (10000 ms)', 519.0)]
        self.assertIn('log holes (no record at all', '\n'.join(report.head(session.build('B', recs), report.Pulled())))

    def test_a_pull_judges_every_device_check_with_the_owed_lanes_last(self):
        with tempfile.TemporaryDirectory() as d:
            logs = Path(d) / 'logs.ndjson'
            record = {
                'eventType': 'logEvent',
                'timestamp': '2026-11-26 09:00:01.000000+0000',
                'bootUUID': 'B1',
                'processImagePath': '/kernel.development',
                'messageType': 'Default',
                'eventMessage': 'TegraSPI stats: SPI4 5000 ms: 100 blocks (20/s), 50 packed, 4000 bytes in 1000 FIFO '
                'words each way, 2 failed',
            }
            logs.write_text(json.dumps(record) + '\n')
            text = perf.write_checks(str(logs), None, Path(d), ndjson=True).read_text()
        self.assertIn('owed.power.spi4-failed', text)
        self.assertIn('owed-summary', text)
        self.assertRegex(text, r'owed\.power\s+big-power\s+FAIL')

    def test_a_pull_takes_the_log_archive_and_the_crash_folder(self):
        cmds = perf.pull_commands(perf.Path('/s'), 'abc', 600)
        self.assertEqual(cmds[0], ['idevicesyslog', '-u', 'abc', 'archive', '/s/archive.tar', '--age-limit', '600'])
        self.assertEqual(cmds[1], ['idevicecrashreport', '-u', 'abc', '-k', '-e', '/s/crash'])


class ArchiveRootTest(unittest.TestCase):
    def test_the_archive_is_found_at_the_top_or_one_folder_down(self):
        with tempfile.TemporaryDirectory() as d:
            top, nested = perf.Path(d, 'top'), perf.Path(d, 'nested')
            (nested / 'x.logarchive').mkdir(parents=True)
            top.mkdir()
            for root in (top, nested / 'x.logarchive'):
                (root / 'Info.plist').write_text('')
            self.assertEqual(perf.archive_root(top), top)
            self.assertEqual(perf.archive_root(nested), nested / 'x.logarchive')


class SourceTest(unittest.TestCase):
    def test_ndjson_lines_become_records_and_name_their_images(self):
        line = json.dumps(
            {
                'eventType': 'logEvent',
                'timestamp': '2026-11-23 00:13:20.703000+0000',
                'machTimestamp': 5,
                'bootUUID': 'B',
                'processImagePath': '/kernel.development',
                'processImageUUID': 'e181',
                'senderImagePath': '/System/Library/Extensions/X.kext/X',
                'senderImageUUID': 'abcd',
                'eventMessage': 'SwitchFrame: x',
            }
        )
        images: dict[str, str] = {}
        (r,) = source.parse_lines([line, '{"eventType": "statedumpEvent"}'], images)
        self.assertEqual((r.proc, r.msg, r.time), ('kernel', 'SwitchFrame: x', T0 + timedelta(milliseconds=703)))
        self.assertEqual(images['ABCD'], '/System/Library/Extensions/X.kext/X')


class Stage3DecodeAndSymbolTest(unittest.TestCase):
    def _make_symbols(self, root: str) -> symbols.Symbols:
        with open(os.path.join(root, 'dsc.tsv'), 'w') as f:
            f.write('I\t180000000\t180200000\t/System/Library/Frameworks/CoreFoundation\n')
            f.write('I\t192000000\t192300000\t/System/Library/Frameworks/QuartzCore\n')
            f.write('S\t180000000\tCoreFoundation\tCFRunLoopRun\n')
            f.write('S\t192000040\tQuartzCore\tCA::Render::Server::render\n')
        with open(os.path.join(root, 'kernel.tsv'), 'w') as f:
            f.write('fffffe0007001000 T _vm_map_clip_unnest\n')
        with open(os.path.join(root, 'SpringBoard.tsv'), 'w') as f:
            f.write('S\t1000\tSBMainLoop\n')
        return symbols.load(symbols_dir=root)

    def test_shared_cache_functions_resolve_through_their_index(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / 'dsc.tsv').write_text('I\t180404000\t180452000\tlibobjc.A.dylib\n')
            with gzip.open(root / symbols.FUNCTIONS, 'wt') as f:
                f.write('I\t180404000\t180452000\tlibobjc.A.dylib\n')
                f.write('S\t180405264\tlibobjc.A.dylib\t_overflow_retain\n')
                f.write('S\t180405240\tlibobjc.A.dylib\t_customRR_retain\n')
            syms = symbols.load(symbols_dir=d)
            self.assertEqual(syms.dsc_addr(0x180405248), 'libobjc.A.dylib _customRR_retain+0x8')
            self.assertEqual(syms.dsc_addr(0x180405264), 'libobjc.A.dylib _overflow_retain')
            self.assertEqual(syms.dsc_addr(0x180404010), 'libobjc.A.dylib+0x10')
            self.assertTrue((root / symbols.FUNCTION_INDEX).is_file())
            self.assertEqual(symbols.load(symbols_dir=d).dsc_addr(0x180405244), 'libobjc.A.dylib _customRR_retain+0x4')
        row = '0x180405240:\t__TEXT.__text\t_customRR_retain\t/usr/lib/libobjc.A.dylib'
        self.assertEqual(symbols.ipsw_symbol(row), 'S\t180405240\tlibobjc.A.dylib\t_customRR_retain\n')
        self.assertIsNone(symbols.ipsw_symbol('0x180500000:\t__DATA.__data\t_x\t/usr/lib/libobjc.A.dylib'))
        self.assertEqual(
            symbols._parse_ipsw_entry({'load_address': 0x180404000, 'name': '/usr/lib/libobjc.A.dylib'}),
            (0x180404000, 0, 'libobjc.A.dylib'),
        )

    def test_symbols_resolve_dsc_kernel_and_bundle_with_fallback(self):
        with tempfile.TemporaryDirectory() as d:
            syms = self._make_symbols(d)
            macho = Path(d, 'fake_app')
            header = struct.pack('<IIIIIIII', symbols.MH_MAGIC_64, 0, 0, 2, 1, 72, 0, 0)
            seg = struct.pack(
                '<II16sQQQQIIII', symbols.LC_SEGMENT_64, 72, b'__TEXT', 0x100000000, 0x4000, 0, 0, 0, 0, 0, 0
            )
            macho.write_bytes(header + seg)
            self.assertEqual(symbols.macho_text_base(macho), 0x100000000)
        self.assertEqual(syms.dsc_addr(0x180000040), 'CoreFoundation CFRunLoopRun+0x40')
        self.assertEqual(syms.dsc_addr(0x192000010), 'QuartzCore+0x10')
        self.assertEqual(syms.dsc_addr(0x250000010), 'dsc 0x250000010')
        self.assertEqual(syms.dsc_region(0x1920), 'QuartzCore (dsc 1920)')
        self.assertEqual(syms.kernel_addr(0xFFFFFE0007001018), '_vm_map_clip_unnest+0x18')
        self.assertEqual(syms.kernel_addr(0xFFFFFE0009000000), 'k 0xfffffe0009000000')
        self.assertEqual(syms.image_addr('SpringBoard', 0x1020), 'SpringBoard SBMainLoop+0x20')
        self.assertEqual(syms.image_addr('OtherApp', 0x20), 'OtherApp+0x20')
        self.assertEqual(symbols.Symbols().dsc_addr(0x180000040), 'dsc 0x180000040')

    def test_kprof_attributes_cpu_floor_and_window_with_thread_roles(self):
        quiet_power = (
            'SwitchPower: 10000 ms: cpu0 c7 0.0% (0, 0 skipped, 0 frame wakes) wfi 70.0% busy 30.0% '
            'cpu1 c7 0.0% (0, 0 skipped, 0 frame wakes) wfi 80.0% busy 20.0%'
        )
        busy_power = (
            'SwitchPower: 10000 ms: cpu0 c7 0.0% (0, 0 skipped, 0 frame wakes) wfi 10.0% busy 90.0% '
            'cpu1 c7 0.0% (0, 0 skipped, 0 frame wakes) wfi 10.0% busy 90.0%'
        )
        kprof_recs = [
            rec(quiet_power, 10.0),
            rec(CORES, 10.0),
            rec(TASKS, 10.001),
            rec('SwitchKprof: 10000 ms, 4 cpus busy/irq %: 30/1.0 20/0.5', 10.002),
            rec(
                'SwitchKprof tasks (user+kernel samples=exact CPU): SpringBoard[30] 120+40=650ms, '
                'backboardd[62] 80+20=410ms',
                10.003,
            ),
            rec(
                'SwitchKprof user 1/2: SpringBoard[30] 120 (main 70, worker 50): '
                'main: dsc 180000040 45, main+ 1020 15, other 10; '
                'worker: dsc 192000040 35, other 15;',
                10.004,
            ),
            rec(
                'SwitchKprof user 2/2: backboardd[62] 80: dsc 192000040 50, other 30;',
                10.005,
            ),
            rec(
                'SwitchKprof images: SpringBoard[30] main 70/120: dsc 1800 55, main+ 0 15, rest 0; lost 0',
                10.006,
            ),
            rec(busy_power, 20.0),
            rec(busy_power, 30.0),
            rec(busy_power, 40.0),
        ]
        with tempfile.TemporaryDirectory() as d:
            syms = self._make_symbols(d)
        s = session.build('B', kprof_recs)
        self.assertEqual(len(s.kprof), 1)
        rendered = report.render(s, report.Pulled(syms=syms))
        self.assertIn('CPU floor (quietest quarter, 1 windows): mean 0.50 busy cores', rendered)
        self.assertIn('floor processes (SwitchSched tasks): SpringBoard 150 ms/s, backboardd 70 ms/s', rendered)
        self.assertIn('floor processes (SwitchKprof): SpringBoard 65 ms/s, backboardd 41 ms/s', rendered)
        self.assertIn(
            'kprof user SpringBoard (main) (70 samples): CoreFoundation CFRunLoopRun+0x40 45, '
            'SpringBoard SBMainLoop+0x20 15',
            rendered,
        )
        self.assertIn(
            'kprof user SpringBoard (worker) (50 samples): QuartzCore CA::Render::Server::render 35', rendered
        )
        self.assertIn('kprof user backboardd (80 samples): QuartzCore CA::Render::Server::render 50', rendered)
        self.assertIn('kprof images SpringBoard (main) (70 samples): CoreFoundation (dsc 1800) 55', rendered)
        w = windows.Window(
            'launch', 'SpringBoard', T0 + timedelta(seconds=2), T0 + timedelta(seconds=11), 30, 'SpringBoard'
        )
        doss = '\n'.join(session.dossier(s, w, syms))
        self.assertIn('kprof tasks: SpringBoard 65 ms/s (160s), backboardd 41 ms/s (100s)', doss)
        self.assertIn('kprof user SpringBoard (main) (70 samples): CoreFoundation CFRunLoopRun+0x40 45', doss)

    def test_launch_window_shows_pageins_and_shared_cache_unnests(self):
        ask = Rec(
            T0 + timedelta(seconds=1),
            0,
            'B',
            'SpringBoard',
            30,
            1,
            'com.apple.FrontBoard',
            '',
            '',
            '',
            0,
            'Asked to bootstrap a new process with identity: app<com.apple.Preferences(x)>',
        )
        boot = Rec(
            T0 + timedelta(seconds=1.1),
            0,
            'B',
            'SpringBoard',
            30,
            1,
            'com.apple.FrontBoard',
            '',
            '',
            '',
            0,
            'Bootstrap success! <FBProcess: 0x1; app<com.apple.Preferences(x)>:659(v1)>',
        )
        ready = Rec(
            T0 + timedelta(seconds=8),
            0,
            'B',
            'SpringBoard',
            30,
            1,
            'com.apple.FrontBoard',
            '',
            '',
            '',
            0,
            'sceneID:com.apple.Preferences-1 content state changed: ready',
        )
        pagein_line = (
            'SwitchSched page-ins (10000 ms): 1200 reads, 96000 KiB asked, 40 ms reading; '
            '1500 waits for pages being read, 1800 ms (longest 116 ms: Preferences[659] on dyld_shared_cache_arm64e); '
            '12 other busy-page waits, 3 ms; untracked 0; XNU page-ins 3400, 120 evicted and paged in again '
            '(ghosts found, 900 added, the last 25600 evictions held); by process '
            '(reads/KiB asked/ms reading/waits/ms waiting): Preferences[659] 800/64000/30/1100/1400, '
            'SpringBoard[30] 190/15200/10/210/400'
        )
        pagein_files = (
            'SwitchSched page-ins by file (10000 ms; reads/KiB asked/ms reading/waits/ms waiting): '
            'dyld_shared_cache_arm64e 900/72000/30/1100/1800, Preferences 80/6400/10/40/100'
        )
        unnest_line = (
            'SwitchUnnest: Preferences[659] unnest 0x180000000->0x180200000: call 74 from user pc dsc 0x180000040 '
            'lr dsc 0x192000040; kernel 0xfffffe0007001018'
        )
        recs = [ask, boot, rec(unnest_line, 4.0), ready, rec(pagein_line, 7.5), rec(pagein_files, 7.501)]
        with tempfile.TemporaryDirectory() as d:
            syms = self._make_symbols(d)
        s = session.build('B', recs)
        self.assertEqual(len(s.pageins), 1)
        self.assertEqual(len(s.unnests), 1)
        self.assertEqual(vm.call_label(74), 'mprotect (74)')
        rendered = report.render(s, report.Pulled(syms=syms))
        self.assertIn('## Page-ins and shared-cache unnests', rendered)
        self.assertIn(
            'page-ins (1 window): 1200 reads (96000 KiB, 40 ms reading), '
            '1500 busy-page waits (1800 ms waiting, 120 refaults)',
            rendered,
        )
        self.assertIn('by file: dyld_shared_cache_arm64e 72000 KiB/1830 ms, Preferences 6400 KiB/110 ms', rendered)
        self.assertIn('shared-cache unnests: 1 (2.0 MiB); by process: Preferences 1; calls: mprotect (74) 1', rendered)
        self.assertIn(
            'Preferences mprotect (74) pc CoreFoundation CFRunLoopRun+0x40 lr QuartzCore CA::Render::Server::render x1',
            rendered,
        )

    def test_ips_symbolizes_watchdog_frames_when_tables_given(self):
        body = {
            'uptime': 130,
            'pid': 30,
            'stackshot': {
                'binaryImages': [
                    ['00000000-0000-0000-0000-000000000000', 0x180000000, 'S'],
                    ['11111111-1111-1111-1111-111111111111', 0xFFFFFE0007000000, 'U'],
                ],
                'processByPid': {
                    '30': {
                        'pid': 30,
                        'procname': 'SpringBoard',
                        'userTimeTask': 12.8,
                        'systemTimeTask': 6.4,
                        'waitInfo': [],
                        'threadById': {
                            '520': {
                                'state': ['TH_WAIT'],
                                'snapshotFlags': ['kThreadMain'],
                                'kernelFrames': [[1, 0x1018]],
                                'userFrames': [[0, 0x40]],
                            },
                        },
                    }
                },
            },
        }
        with tempfile.TemporaryDirectory() as d:
            syms = self._make_symbols(d)
            r = ips.Report(os.path.join(d, 'sb.ips'), '409', 'SpringBoard', '2026-11-23 00:00:00', 'B', body, '24B119')
            text = '\n'.join(ips.summary([r], {'B'}, {}, ('SpringBoard',), syms))
        self.assertIn('_vm_map_clip_unnest+0x18', text)
        self.assertIn('CoreFoundation CFRunLoopRun+0x40', text)


if __name__ == '__main__':
    unittest.main()
