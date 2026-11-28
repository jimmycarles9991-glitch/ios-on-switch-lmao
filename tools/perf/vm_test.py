"""Tests for vm.py's SwitchUnnest parsing and symbolization."""

from __future__ import annotations

import os
import tempfile
import unittest
from datetime import UTC, datetime

from tools.perf import symbols, vm
from tools.perf.source import Rec

T0 = datetime(2026, 11, 26, 12, 0, 0, tzinfo=UTC)


def _kernel_rec(msg: str, proc: str = 'kernel') -> Rec:
    return Rec(
        time=T0,
        mach=0,
        boot='B',
        proc=proc,
        pid=0,
        tid=1,
        subsystem='',
        category='',
        name='',
        kind='',
        ident=0,
        msg=msg,
    )


def _make_symbols(root: str) -> symbols.Symbols:
    with open(os.path.join(root, 'dsc.tsv'), 'w') as f:
        f.write('I\t180000000\t180200000\t/System/Library/Frameworks/CoreFoundation\n')
        f.write('I\t192000000\t192300000\t/System/Library/Frameworks/QuartzCore\n')
        f.write('S\t180000000\tCoreFoundation\tCFRunLoopRun\n')
        f.write('S\t192000040\tQuartzCore\tCA::Render::Server::render\n')
    with open(os.path.join(root, 'SpringBoard.tsv'), 'w') as f:
        f.write('S\t1000\tSBMainLoop\n')
    return symbols.load(symbols_dir=root)


class UnnestParsingAndSummaryTest(unittest.TestCase):
    def test_legacy_unnest_line_without_frames_remains_compatible(self):
        line = (
            'SwitchUnnest: Preferences[659] unnest 0x180000000->0x180200000: call 74 '
            'from user pc dsc 0x180000040 lr dsc 0x192000040; kernel 0xfffffe0007001018'
        )
        events = vm.unnest_lines([_kernel_rec(line), _kernel_rec(line, proc='Preferences')])
        self.assertEqual(len(events), 1)
        e = events[0]
        self.assertEqual((e.proc, e.pid, e.call), ('Preferences', 659, 74))
        self.assertEqual((e.start, e.end, e.size), (0x180000000, 0x180200000, 0x200000))
        self.assertEqual((e.pc, e.lr), ('dsc 0x180000040', 'dsc 0x192000040'))
        self.assertEqual(e.frames, ())
        self.assertEqual(e.kernel, (0xFFFFFE0007001018,))

        self.assertEqual(
            vm.unnest_summary(events)[1],
            '  Preferences mprotect (74) pc dsc 0x180000040 lr dsc 0x192000040 x1',
        )
        with tempfile.TemporaryDirectory() as d:
            syms = _make_symbols(d)
        self.assertEqual(
            vm.unnest_summary(events, syms=syms)[1],
            '  Preferences mprotect (74) pc CoreFoundation CFRunLoopRun+0x40 '
            'lr QuartzCore CA::Render::Server::render x1',
        )

    def test_unnest_line_with_user_frames_parses_and_symbolizes_chain(self):
        line = (
            'SwitchUnnest: SpringBoard[30] unnest 0x180000000->0x180100000: call 74 '
            'from user pc dsc 0x180000040 lr dsc 0x192000040, '
            'frames dsc 0x192000050 main+0x1020 0x104008020 dsc 0x180000080; '
            'kernel 0xfffffe0007001018 0xfffffe0007002040'
        )
        other = (
            'SwitchUnnest: SpringBoard[30] unnest 0x180100000->0x180200000: call 74 '
            'from user pc dsc 0x180000040 lr dsc 0x192000040, '
            'frames main+0x1020; kernel 0xfffffe0007001018'
        )
        events = vm.unnest_lines([_kernel_rec(line), _kernel_rec(line), _kernel_rec(other)])
        self.assertEqual(len(events), 3)
        self.assertEqual(
            events[0].frames,
            ('dsc 0x192000050', 'main+0x1020', '0x104008020', 'dsc 0x180000080'),
        )
        self.assertEqual(events[0].kernel, (0xFFFFFE0007001018, 0xFFFFFE0007002040))
        self.assertEqual(events[2].frames, ('main+0x1020',))

        raw_summary = vm.unnest_summary(events)
        self.assertEqual(
            raw_summary[1],
            '  SpringBoard mprotect (74) pc dsc 0x180000040 lr dsc 0x192000040 '
            '<- dsc 0x192000050 <- main+0x1020 <- 0x104008020 <- dsc 0x180000080 x2',
        )
        self.assertEqual(
            raw_summary[2],
            '  SpringBoard mprotect (74) pc dsc 0x180000040 lr dsc 0x192000040 <- main+0x1020 x1',
        )

        with tempfile.TemporaryDirectory() as d:
            syms = _make_symbols(d)
        sym_summary = vm.unnest_summary(events, syms=syms)
        self.assertEqual(
            sym_summary[1],
            '  SpringBoard mprotect (74) pc CoreFoundation CFRunLoopRun+0x40 '
            'lr QuartzCore CA::Render::Server::render '
            '<- QuartzCore CA::Render::Server::render+0x10 '
            '<- SpringBoard SBMainLoop+0x20 <- 0x104008020 <- CoreFoundation CFRunLoopRun+0x80 x2',
        )
        self.assertEqual(
            sym_summary[2],
            '  SpringBoard mprotect (74) pc CoreFoundation CFRunLoopRun+0x40 '
            'lr QuartzCore CA::Render::Server::render <- SpringBoard SBMainLoop+0x20 x1',
        )

    def test_empty_frames_and_missing_user_regs_parse_cleanly(self):
        lines = [
            (
                'SwitchUnnest: kernel_task[0] unnest 0x180000000->0x180004000: call 0 '
                'from user pc - lr -; kernel 0xfffffe0007001018'
            ),
            (
                'SwitchUnnest: dyld[12] unnest 0x180000000->0x180004000: call -14 '
                'from user pc 0x100004000 lr 0x100004020, frames; kernel'
            ),
        ]
        events = vm.unnest_lines([_kernel_rec(msg) for msg in lines])
        self.assertEqual(len(events), 2)
        self.assertEqual((events[0].pc, events[0].lr, events[0].frames), ('-', '-', ()))
        self.assertEqual((events[1].call, events[1].frames, events[1].kernel), (-14, (), ()))


if __name__ == '__main__':
    unittest.main()
