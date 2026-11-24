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
        self.assertIn('its thread: on a core 300 ms of the last 1000', text)
        self.assertIn('leaves: dsc 0x180000040 1', text)
        self.assertIn('most common stack (1): dsc 0x180000040 < SwitchHWTest+0x10 < dsc 0x180000080', text)
        self.assertIn('GPU: 1 jobs ended', text)
        self.assertIn('longest job channel 7 seqno 5 (compositor) 3.0 ms', text)

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
