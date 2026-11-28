"""tools/perf's reading of a flight recorder dump, the ring and the stackshot beside it.

The ring is RAW kdebug as kdbg_dump_trace_to_file writes it; the stackshot's names and images are as XNU's kcdata.py
prints them.
"""

import json
import os
import struct
import tempfile
import unittest
from datetime import UTC, datetime, timedelta

from tools.perf import cpu, dumps, ktrace, report, session, symbols, windows
from tools.perf.source import TICK_HZ, Rec

MAIN, RENDER, SUBMITTER = 200, 100, 300
DSC_BASE, DSC_SLIDE = 0x190000000, 0x10000000
APP_BASE = 0x100000000
APP_UUID = '0123456789abcdef0123456789abcdef'


def tick(ms: float) -> int:
    return 10_000_000_000 + int(ms * TICK_HZ / 1000)


def event(ms: float, debugid: int, args: tuple, tid: int, cpu_id: int = 0) -> bytes:
    a = (*args, 0, 0, 0, 0)[:4]
    return ktrace.EVENT.pack(tick(ms), *a, tid, debugid, cpu_id, 0)


def vfs_lookup_events(ms: float, vnode: int, path: str, tid: int) -> list[bytes]:
    raw = path.encode('utf-8')
    first_chunk = raw[:24].ljust(24, b'\x00')
    w0, w1, w2 = struct.unpack('<QQQ', first_chunk)
    rest = raw[24:]
    flag = ktrace.FUNC_START | (ktrace.FUNC_END if not rest else 0)
    out = [event(ms, ktrace.VFS_LOOKUP | flag, (vnode, w0, w1, w2), tid)]
    pos = 0
    step = 0.01
    while pos < len(rest):
        chunk = rest[pos : pos + 32].ljust(32, b'\x00')
        pos += 32
        end_flag = ktrace.FUNC_END if pos >= len(rest) else 0
        out.append(event(ms + step, ktrace.VFS_LOOKUP | end_flag, struct.unpack('<QQQQ', chunk), tid))
        step += 0.01
    return out


def pack_ring(events: list[bytes], tod_secs: int = 1_795_400_000) -> bytes:
    head = ktrace.HEADER.pack(ktrace.RAW_VERSION1, 384, tod_secs, 0)
    return head + bytes(16384 - len(head)) + b''.join(events)


def ring() -> bytes:
    """Return a ring as the kernel writes it with no thread map: 384 padding entries, events from 16 KiB."""
    return pack_ring(
        [
            event(0, ktrace.MACH_SCHED, (0, MAIN, 31, 47), RENDER),
            event(100, ktrace.code(37, 2, 6), (0, 3), MAIN),
            event(100.01, ktrace.code(37, 2, 4), (DSC_BASE + 0x40, APP_BASE + 0x10, DSC_BASE + 0x80, 0), MAIN),
            event(150, ktrace.SUBMIT, (7, 5, 1), SUBMITTER),
            event(260, ktrace.GPU_JOB, ((5 << 32) | (1 << 16) | 7, 1_000_000, 4_000_000), 0),
            event(300, ktrace.MACH_SCHED, (0, RENDER, 47, 31), MAIN),
            event(400, ktrace.TRIGGER, (2, MAIN, 412), 0),
            event(401, ktrace.DUMP | ktrace.FUNC_START, (2, 0), 0),
        ]
    )


def kcdata() -> dict:
    return {
        'kcdata_stackshot': {
            'task_snapshots': {
                '1': {
                    'task_snapshot': {'ts_pid': 42, 'ts_p_comm': 'SwitchHWTest'},
                    'thread_snapshots': {'200': {'thread_snapshot': {'ths_thread_id': MAIN}, 'pth_name': 'main'}},
                    'dyld_load_info': [{'imageLoadAddress': APP_BASE, 'imageUUID': list(bytes.fromhex(APP_UUID))}],
                    'shared_cache_dyld_load_info': {'imageSlidBaseAddress': DSC_BASE, 'imageLoadAddress': DSC_SLIDE},
                },
            }
        }
    }


class DumpTest(unittest.TestCase):
    def test_a_hang_dump_names_its_thread_and_its_stacks(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, 'Switch-recorder-0')
            with open(path + '.ktrace', 'wb') as f:
                f.write(ring())
            with open(path + '.kcdata', 'wb') as f:
                f.write(b'kcdata')
            decoder = os.path.join(d, 'kcdata.py')
            with open(decoder, 'w') as f:
                f.write(f'import json\nprint(json.dumps({json.dumps(kcdata())}))\n')
            self.assertEqual(dumps.find(d), [(path + '.ktrace', path + '.kcdata')])
            uuid = '01234567-89AB-CDEF-0123-456789ABCDEF'
            text = '\n'.join(dumps.summary(path + '.ktrace', path + '.kcdata', decoder, {uuid: '/x/SwitchHWTest'}))
        self.assertIn('Switch-recorder-0.ktrace: hang, thread SwitchHWTest[42] main (200), value 412', text)
        self.assertIn('its thread: on a core 300 ms of the last 400', text)
        self.assertIn('the ring reaches 400 ms before the trigger: the capture before it read the rest', text)
        self.assertIn('leaves: dsc 0x180000040 1', text)
        self.assertIn('most common stack (1): dsc 0x180000040 < SwitchHWTest+0x10 < dsc 0x180000080', text)
        self.assertIn('GPU: 1 submissions ended', text)
        self.assertIn('longest submission channel 7 seqno 5 (compositor) 4.9 ms', text)

    def test_a_hang_dump_symbolizes_with_dsc_and_bundle_tables(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, 'Switch-recorder-0')
            with open(path + '.ktrace', 'wb') as f:
                f.write(ring())
            with open(path + '.kcdata', 'wb') as f:
                f.write(b'kcdata')
            decoder = os.path.join(d, 'kcdata.py')
            with open(decoder, 'w') as f:
                f.write(f'import json\nprint(json.dumps({json.dumps(kcdata())}))\n')
            sym_dir = os.path.join(d, 'syms')
            os.mkdir(sym_dir)
            with open(os.path.join(sym_dir, 'dsc.tsv'), 'w') as f:
                f.write('I\t180000000\t180100000\t/System/Library/Frameworks/CoreFoundation\n')
                f.write('S\t180000020\tCoreFoundation\tCFRunLoopRun\n')
            with open(os.path.join(sym_dir, 'SwitchHWTest.tsv'), 'w') as f:
                f.write('S\t0\tdo_work\n')
            syms = symbols.load(symbols_dir=sym_dir)
            uuid = '01234567-89AB-CDEF-0123-456789ABCDEF'
            text = '\n'.join(
                dumps.summary(path + '.ktrace', path + '.kcdata', decoder, {uuid: '/x/SwitchHWTest'}, syms)
            )
        self.assertIn('leaves: CoreFoundation CFRunLoopRun+0x20 1', text)
        self.assertIn(
            'most common stack (1): CoreFoundation CFRunLoopRun+0x20 < SwitchHWTest do_work+0x10 '
            '< CoreFoundation CFRunLoopRun+0x60',
            text,
        )

    def test_a_ring_that_is_not_raw_kdebug_is_refused_and_tod_checks_magic(self):
        with self.assertRaises(ValueError):
            ktrace.parse(bytes(64))
        with self.assertRaises(ValueError):
            ktrace.parse(bytes(4))
        with tempfile.TemporaryDirectory() as d:
            bad_magic = os.path.join(d, 'Switch-recorder-0.ktrace')
            with open(bad_magic, 'wb') as f:
                f.write(ktrace.HEADER.pack(0xDEADBEEF, 0, 1_795_400_000, 0))
            self.assertEqual(ktrace.tod(bad_magic), 0)
            trunc = os.path.join(d, 'Switch-recorder-1.ktrace')
            t0 = datetime(2026, 11, 23, 0, 15, 0, tzinfo=UTC)
            with open(trunc, 'wb') as f:
                f.write(ktrace.HEADER.pack(ktrace.RAW_VERSION1, 9999, int(t0.timestamp()), 0))
            self.assertEqual(ktrace.tod(trunc), int(t0.timestamp()))
            rec = Rec(t0, 0, 'B', 'kernel', 0, 0, '', '', '', '', 0, 'SwitchRecorder: dump 1')
            pulled = report.Pulled(dumps=[(trunc, trunc[:-7] + '.kcdata')])
            rendered = report.render(session.build('B', [rec]), pulled)
        self.assertIn('Switch-recorder-1.ktrace: unreadable dump', rendered)

    def test_a_submission_of_several_batches_runs_from_its_first_start_to_its_last_end(self):
        """Slot 7 of 25 Nov: SpringBoard's seqnos 143-210 were one 50.2 ms submission (hitch-shots-codex/jobs.txt)."""
        job = ktrace.GPU_JOB
        events = [
            event(1, ktrace.SUBMIT, (12, 210, 0), SUBMITTER),
            event(60, job, ((143 << 32) | 12, 1_000_000, 0), 0),
            *[event(60 + i * 0.1, job, (((143 + i) << 32) | 12, 0, 0), 0) for i in range(1, 67)],
            event(70, job, ((210 << 32) | 12, 0, 1_000_000 + int(50.2e6 * ktrace.PTIMER_PER_NS)), 0),
        ]
        (sub,) = ktrace.submissions(ktrace.parse(pack_ring(events)).events)
        self.assertEqual((sub.channel, sub.first, sub.last, sub.tid), (12, 143, 210, SUBMITTER))
        self.assertAlmostEqual(sub.ms, 50.2, places=3)

    def test_the_compositors_waits_name_the_process_and_a_stale_start_is_bounded(self):
        maps = 400
        job, wait = ktrace.GPU_JOB, ktrace.GPU_WAIT
        events = [
            event(1, ktrace.SUBMIT, (21, 1, 0), maps),
            event(2, ktrace.SUBMIT, (21, 2, 0), maps),
            event(3, ktrace.SUBMIT, (2, 7, 1), SUBMITTER),
            event(10, wait, (2 | (1 << 16) | (21 << 32), 2, 7), SUBMITTER),
            event(11, wait, (2 | (3 << 16), 9, 7), SUBMITTER),
            event(100, job, ((1 << 32) | 21, 1_000_000, 2_000_000), 0),
            event(200, job, ((2 << 32) | 21, 5, 2_000_000 + int(10e6 * ktrace.PTIMER_PER_NS)), 0),
            event(300, ktrace.TRIGGER, (4, 0, 0), 0),
        ]
        ring = ktrace.parse(pack_ring(events))
        second = ktrace.submissions(ring.events)[1]
        self.assertTrue(second.bounded)
        self.assertAlmostEqual(second.ms, 10.0, places=3)
        text = '\n'.join(dumps.gpu(ring, dumps.Names(), 0, tick(300)))
        self.assertIn('GPU waits behind unsettled work: channel 2 on channel 21 (?[0]) 1, channel 2 on VIC 1', text)
        self.assertIn("GPU: from their channel's previous end, a stale start stamp: channel 21 1", text)

    def test_a_submission_waiting_for_ring_room_says_how_long_and_how_full(self):
        room, done = ktrace.GPU_ROOM, ktrace.GPU_ROOM_DONE
        events = [
            event(10, room, (21, (5 << 32) | 4, (90 << 32) | 82), SUBMITTER),
            event(310, done, (21, (6 << 32) | 4, 90 << 32), SUBMITTER),
        ]
        (line,) = dumps.room_lines(ktrace.parse(pack_ring(events)), dumps.Names(), 0, tick(400))
        self.assertEqual(
            line,
            f'GPU ring full: 1 submissions waited for room, ?[0] ({SUBMITTER}) channel 21 300.0 ms (GET 5 PUT 4, '
            '8 jobs unfinished)',
        )

    def test_a_submission_older_than_the_ring_is_left_out(self):
        """Maps' channel 21 on 25 Nov: start stamps about 1188 s before their end, in a ring of a second or two."""
        job = ktrace.GPU_JOB
        events = [
            event(1, ktrace.SUBMIT, (21, 9, 0), SUBMITTER),
            event(900, job, ((9 << 32) | 21, 5, 5 + int(1188e9 * ktrace.PTIMER_PER_NS)), 0),
            event(1000, ktrace.TRIGGER, (2, MAIN, 300), 0),
        ]
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'Switch-recorder-2.ktrace')
            with open(p, 'wb') as f:
                f.write(pack_ring(events))
            text = '\n'.join(dumps.summary(p, p[:-7] + '.kcdata', None, {}))
        self.assertIn('GPU: no submission ended in the window', text)
        self.assertIn('GPU: left out, a start stamp older than the ring: channel 21 1', text)

    def test_a_hang_names_its_faults_and_the_reads_its_page_ins_waited_behind(self):
        """Dump 4 of 25 Nov: SpringBoard's main faulted 145 ms behind the pack warm's tier-0 passive 512 KiB reads."""
        warm = 3360
        fault = ktrace.VM_FAULT
        tiered = ktrace.code(3, 2, ktrace.DKIO_READ | ktrace.DKIO_PAGING | ktrace.DKIO_PASSIVE)
        done = ktrace.code(3, 2, ktrace.DKIO_DONE | ktrace.DKIO_READ | ktrace.DKIO_PAGING)
        own = ktrace.code(3, 2, ktrace.DKIO_READ | ktrace.DKIO_PAGING | (3 << ktrace.DKIO_TIER_SHIFT))
        events = [
            event(100, fault | ktrace.FUNC_START, (0, 0x1000), MAIN),
            event(101, tiered, (0xB1, 1, 0, 512 << 10), warm),
            event(102, own, (0xB2, 1, 0, 16 << 10), MAIN),
            event(120, done, (0xB1, 0, 0, 0), 0),
            event(125, done, (0xB2, 0, 0, 0), 0),
            event(130, fault | ktrace.FUNC_END, (0, 0x1000, 0, 2), MAIN),
            event(140, fault | ktrace.FUNC_START, (0, 0x2000), MAIN),
            event(141, fault | ktrace.FUNC_END, (0, 0x2000, 0, 1), MAIN),
            event(400, ktrace.TRIGGER, (2, MAIN, 300), 0),
        ]
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'Switch-recorder-2.ktrace')
            with open(p, 'wb') as f:
                f.write(pack_ring(events))
            text = '\n'.join(dumps.summary(p, p[:-7] + '.kcdata', None, {}))
        self.assertIn('its faults: 2, page-in 30.0 ms, zero fill 1.0 ms; the longest 30.0 ms', text)
        self.assertIn('disk reads during its page-ins: 2, 528 KiB', text)
        self.assertIn(f'?[0] ({warm}) tier 0 passive 512 KiB, its own tier 3 16 KiB', text)
        self.assertIn('by tier (p: passive): 0p 1, 3 1', text)

    def test_a_thread_that_ended_before_the_stackshot_is_named_from_the_ring(self):
        def words(text: str) -> tuple[int, ...]:
            return struct.unpack('<QQQQ', text.encode().ljust(32, b'\0'))

        events = [
            event(1, ktrace.TRACE_NEWTHREAD, (777, 31, 0, 99), MAIN),
            event(
                1.01, ktrace.TRACE_NEWTHREAD_NAME | ktrace.FUNC_START | ktrace.FUNC_END, words('fileproviderd'), MAIN
            ),
            event(2, ktrace.TRACE_THREADNAME | ktrace.FUNC_START | ktrace.FUNC_END, words('com.apple.fp.sync'), 777),
            event(3, ktrace.MACH_SCHED, (0, 777, 31, 31), MAIN),
            event(50, ktrace.MACH_SCHED, (0, MAIN, 31, 31), 777),
            event(100, ktrace.TRIGGER, (2, MAIN, 300), 0),
        ]
        names = ktrace.thread_names(ktrace.parse(pack_ring(events)).events)
        self.assertEqual(names[777], (31, 'fileproviderd', 'com.apple.fp.sync'))
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'Switch-recorder-2.ktrace')
            with open(p, 'wb') as f:
                f.write(pack_ring(events))
            text = '\n'.join(dumps.summary(p, p[:-7] + '.kcdata', None, {}))
        self.assertIn('fileproviderd[31] com.apple.fp.sync (777) 47 ms', text)

    def test_the_sd_queues_picks_say_who_went_first_and_how_long_the_head_waited(self):
        pick = ktrace.SD_PICK
        events = [
            event(10, pick, (100, 64 | (2 << 32), 3 | (40 << 16), 900), 0),
            event(20, pick, (900, 8 | (1 << 32), 0x100 | (39 << 16), 12_500), 0),
            event(30, pick, (500, 16 | (1 << 32), 1 | 0x200 | (3 << 16), 40), 0),
        ]
        ring = ktrace.parse(pack_ring(events))
        (line,) = dumps.sd_lines(ring, 0, tick(100))
        self.assertIn('SD queue: 3 picks (read background 1, read high 1, write default 1), 1 passed older', line)
        self.assertIn('up to 40 queued, the head waiting up to 12.5 ms', line)

    def test_a_stack_handoff_is_a_context_switch(self):
        """thread_invoke's handoff path logs MACH_STACK_HANDOFF, not MACH_SCHED (hitch-shots-codex, 25 Nov)."""
        events = [
            event(0, ktrace.MACH_SCHED, (0, MAIN, 31, 47), RENDER),
            event(10, ktrace.MACH_STACK_HANDOFF, (0, SUBMITTER, 47, 31), MAIN),
            event(30, ktrace.MACH_SCHED, (0, MAIN, 31, 47), SUBMITTER),
            event(40, ktrace.MACH_SCHED, (0, RENDER, 47, 31), MAIN),
        ]
        stretches = ktrace.runs(ktrace.parse(pack_ring(events)).events)
        self.assertAlmostEqual(dumps.ms(ktrace.on_core(stretches[SUBMITTER], 0, tick(40))), 20.0, places=1)
        self.assertAlmostEqual(dumps.ms(ktrace.on_core(stretches[MAIN], 0, tick(40))), 20.0, places=1)

    def test_pageins_and_unnests_decode_with_vnode_paths_and_syscalls(self):
        dsc_path = '/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64e'
        dk_issue = ktrace.code(3, 2, ktrace.DKIO_READ | ktrace.DKIO_PAGING)
        dk_done = ktrace.code(3, 2, ktrace.DKIO_DONE | ktrace.DKIO_READ | ktrace.DKIO_PAGING)
        mprotect = ktrace.code(4, 0x0C, 74)
        events = [
            *vfs_lookup_events(10, 0xAA01, dsc_path, MAIN),
            event(20, ktrace.CL_PAGEIN, (0, 65536, 1_000_000, 0), MAIN),
            event(21, dk_issue, (0xBEEF, 1, 100, 65536), MAIN),
            event(33, dk_done, (0xBEEF, 0xAA01, 0, 0), 0),
            event(50, mprotect | ktrace.FUNC_START, (0x180000000, 0x200000, 3, 0), MAIN),
            event(51, ktrace.VM_PMAP_UNNEST, (0x180000000, 0x180200000, 0, 0), MAIN),
            event(54.5, mprotect | ktrace.FUNC_END, (0, 0, 0, 0), MAIN),
            event(60, ktrace.SHARED_CACHE_LAYOUT | ktrace.FUNC_START, (), MAIN),
            event(68, ktrace.SHARED_CACHE_LAYOUT | ktrace.FUNC_END, (420,), MAIN),
            event(100, ktrace.TRIGGER, (2, MAIN, 300), 0),
            event(101, ktrace.DUMP | ktrace.FUNC_START, (2, 0), 0),
        ]
        raw = pack_ring(events)
        parsed = ktrace.parse(raw)
        self.assertEqual(ktrace.vnode_paths(parsed.events)[0xAA01], dsc_path)
        dk, fs = ktrace.pageins(parsed.events)
        self.assertEqual(len(dk), 1)
        self.assertEqual(dk[0].path, dsc_path)
        self.assertEqual(dk[0].nbytes, 65536)
        self.assertTrue(dk[0].paging)
        self.assertEqual(len(fs), 1)
        uns, layouts = ktrace.unnests(parsed.events)
        self.assertEqual(len(uns), 1)
        self.assertEqual(uns[0].syscall, 'mprotect')
        self.assertEqual(uns[0].size, 0x200000)
        self.assertAlmostEqual(dumps.ms(uns[0].syscall_ticks), 4.5, places=1)
        self.assertEqual(len(layouts), 1)
        self.assertEqual(layouts[0].images, 420)
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'Switch-recorder-2.ktrace')
            with open(p, 'wb') as f:
                f.write(raw)
            text = '\n'.join(dumps.summary(p, p[:-7] + '.kcdata', None, {}))
        self.assertIn('page-ins: 1 FSRW page-ins (64 KiB); 1 DKRW paging reads (64 KiB, 12.0 ms total', text)
        self.assertIn(f'page-in files: {dsc_path} 64 KiB', text)
        self.assertIn('shared-cache unnests: 1 (2048 KiB):', text)
        self.assertIn('in mprotect (4.5 ms)', text)
        self.assertIn('shared-cache layout: 1 maps (8.0 ms)', text)


TESTDATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'testdata')


class RealDumpTest(unittest.TestCase):
    """Dump 4 of boot 2693043C (25 Nov, iOS 27): SpringBoard's task and its main thread's samples, cut from the pair.

    Its stackshot carries the shared cache's slide once at the top level, not in each task.
    """

    def test_shared_cache_frames_take_the_system_slide(self):
        decoder = os.environ.get('KCDATA', '')
        self.assertTrue(os.path.isfile(decoder), f'kcdata.py not found at {decoder!r}')
        path = os.path.join(TESTDATA, 'Switch-recorder-3')
        n = dumps.names(dumps.decode_kcdata(path + '.kcdata', decoder))
        self.assertEqual(n.thread[514][:2], (30, 'SpringBoard'))
        self.assertEqual(n.dsc[30], (0x189D20000, 0x9D20000))
        text = '\n'.join(dumps.summary(path + '.ktrace', path + '.kcdata', decoder, {}))
        self.assertIn('hang, thread SpringBoard[30] (514), value 320', text)
        stack = next(line for line in text.splitlines() if 'most common stack' in line).split(': ', 1)[1]
        self.assertEqual(stack.split(' < ')[:3], ['dsc 0x189ad0398', 'dsc 0x189acfdf0', 'dsc 0x189acfbf8'])
        self.assertTrue(all(frame.startswith('dsc 0x') for frame in stack.split(' < ')))

    def test_a_wait_names_the_thread_or_task_its_turnstile_ends_at(self):
        shot = {
            'task_snapshots': {
                '1': {
                    'task_snapshot': {'ts_pid': 30, 'ts_p_comm': 'SpringBoard'},
                    'thread_snapshots': {'514': {'thread_snapshot': {'ths_thread_id': 514}}},
                    'thread_waitinfo': [{'waiter': 514, 'wait_type': 2, 'wait_flags': 1, 'owner': 6451}],
                    'thread_turnstileinfo': [
                        {'waiter': 514, 'turnstile_context': 1787, 'turnstile_flags': 8, 'number_of_hops': 2}
                    ],
                },
                '2': {
                    'task_snapshot': {'ts_pid': 69, 'ts_p_comm': 'backboardd'},
                    'thread_snapshots': {'1787': {'thread_snapshot': {'ths_thread_id': 1787}, 'pth_name': 'hid'}},
                    'thread_waitinfo': [{'waiter': 606, 'wait_type': 6}],
                    'thread_turnstileinfo': [
                        {'waiter': 606, 'turnstile_context': 30, 'turnstile_flags': 16, 'number_of_hops': 1}
                    ],
                },
            }
        }
        n = dumps.names({'kcdata_stackshot': shot})
        self.assertEqual(n.waited(514), 'a port receive (its reply port), behind backboardd[69] hid (1787) (2 hops)')
        self.assertEqual(n.waited(606), 'a semaphore, behind SpringBoard[30] (1 hops)')
        self.assertEqual(n.waited(1), '')

    def test_a_task_on_another_cache_or_none_is_resolved_by_its_flags(self):
        shot = {
            'shared_cache_dyld_load_info': {'imageSlidBaseAddress': 0x190000000, 'imageLoadAddress': 0x10000000},
            'shared_caches': {
                '2': {
                    'shared_cache_dyld_load_info': {
                        'sharedCacheID': 2,
                        'sharedCacheSlide': 0x4000,
                        'sharedCacheSlidFirstMapping': 0x280004000,
                    }
                }
            },
            'task_snapshots': {
                '1': {'task_snapshot': {'ts_pid': 1, 'ts_ss_flags': dumps.REGION_SYSTEM}},
                '2': {'task_snapshot': {'ts_pid': 2, 'ts_ss_flags': dumps.REGION_OTHER}, 'sharedCacheID': 2},
                '3': {'task_snapshot': {'ts_pid': 3, 'ts_ss_flags': dumps.REGION_NONE}},
            },
        }
        n = dumps.names({'kcdata_stackshot': shot})
        self.assertEqual(n.dsc, {1: (0x190000000, 0x10000000), 2: (0x280004000, 0x4000)})


class MemoryClockTest(unittest.TestCase):
    def test_a_window_names_the_memory_clocks_residency(self):
        t0 = datetime(2026, 11, 23, 0, 15, 26, tzinfo=UTC)
        line = (
            'SwitchPower: 10005 ms: cpu0 c7 0.0% (0, 0 skipped, 0 frame wakes) wfi 32.2% busy 67.8%; EMC 3 moves '
            '204:46.7% 1331:0.9% 1600:52.4%, 2 raises'
        )
        rec = Rec(t0 + timedelta(seconds=10), 0, 'B', 'kernel', 0, 0, '', '', '', '', 0, line)
        s = session.build('B', [rec])
        w = windows.Window('launch', 'x', t0 + timedelta(seconds=2), t0 + timedelta(seconds=9))
        self.assertEqual(
            {k: round(v, 3) for k, v in cpu.mean_emc(w, s.busy).items()}, {204: 0.467, 1331: 0.009, 1600: 0.524}
        )
        self.assertIn('memory clock: 1600 MHz 52%, 204 MHz 47%, 1331 MHz 1%', '\n'.join(session.dossier(s, w)))


if __name__ == '__main__':
    unittest.main()
