"""The running step on a terminal and in a plain log, its rate and time left, and the progress lines tools print."""

import io
import os
import unittest
from unittest import mock

from tools.install import progress
from tools.install.progress import BYTES, PERCENT, Bar, Update


class Clock:
    def __init__(self) -> None:
        self.now = 0.0

    def __call__(self) -> float:
        return self.now


class RateTest(unittest.TestCase):
    def test_no_rate_before_a_second_of_samples(self):
        rate = progress.Rate()
        rate.add(0.0, 0)
        rate.add(0.5, 100)
        self.assertIsNone(rate.per_second())
        rate.add(1.0, 200)
        self.assertEqual(rate.per_second(), 200)

    def test_the_rate_follows_the_last_ten_seconds(self):
        rate = progress.Rate()
        for t in range(11):
            rate.add(t, 10 * t)
        for t in range(11, 31):
            rate.add(t, 100 + 100 * (t - 10))
        self.assertAlmostEqual(rate.per_second(), 100)

    def test_time_left_is_what_remains_at_the_current_rate(self):
        bar = Bar('copy', 1000, BYTES, 0.0)
        bar.add(0.0, 0)
        bar.add(2.0, 200)
        self.assertEqual(bar.eta(), 8.0)

    def test_no_time_left_without_a_total_or_a_rate(self):
        self.assertIsNone(Bar('copy', None, BYTES, 0.0).eta())
        stalled = Bar('copy', 100, BYTES, 0.0)
        stalled.add(0.0, 10)
        stalled.add(5.0, 0)
        self.assertIsNone(stalled.eta())

    def test_a_new_unit_starts_a_new_rate(self):
        bar = Bar('system_cryptex: translating', 100, 'files', 0.0)
        bar.set(0.0, Update(done=0))
        bar.set(10.0, Update(done=100))
        bar.set(11.0, Update(done=0, total=9, unit='dyld cache files'))
        self.assertIsNone(bar.rate.per_second())
        self.assertEqual((bar.done, bar.total, bar.unit), (0, 9, 'dyld cache files'))


class FormatTest(unittest.TestCase):
    def test_sizes(self):
        self.assertEqual(progress.size(512), '512 B')
        self.assertEqual(progress.size(1536), '1.5 KiB')
        self.assertEqual(progress.size(12 << 30), '12 GiB')
        self.assertEqual(progress.size(1 << 40), '1.0 TiB')

    def test_amounts_share_the_totals_unit(self):
        self.assertEqual(progress.amount(1.2 * (1 << 30), 4.5 * (1 << 30), BYTES), '1.2/4.5 GiB')
        self.assertEqual(progress.amount(300 << 20, 4.5 * (1 << 30), BYTES), '0.3/4.5 GiB')
        self.assertEqual(progress.amount(120, 3413, 'files'), '120/3413 files')
        self.assertEqual(progress.amount(5, None, 'files'), '5 files')

    def test_clock(self):
        self.assertEqual(progress.clock(33), '0:33')
        self.assertEqual(progress.clock(3725), '1:02:05')


class LineTest(unittest.TestCase):
    def counted(self) -> Bar:
        bar = Bar('os: translating', 3413, 'files', 0.0)
        bar.set(0.0, Update(done=0))
        bar.set(10.0, Update(done=1000))
        return bar

    def test_a_counted_step_on_a_terminal(self):
        self.assertEqual(
            self.counted().line(10.0, 200),
            'os: translating  [#######-----------------]  29%  1000/3413 files  100 files/s  0:24 left',
        )

    def test_the_line_fits_the_terminal(self):
        self.assertEqual(self.counted().line(10.0, 20), 'os: translating  [#')

    def test_a_counted_step_in_a_log(self):
        self.assertEqual(self.counted().plain(10.0), '  os: translating: 29%, 1000/3413 files, 100 files/s, 0:24 left')

    def test_a_step_without_a_total_shows_its_time_and_what_it_does(self):
        bar = Bar('fsck_apfs disk5s1 (image)', None, None, 0.0)
        bar.set(3.0, Update(note='Checking the fsroot tree.'))
        self.assertEqual(bar.line(12.0, 200), 'fsck_apfs disk5s1 (image)  0:12  Checking the fsroot tree.')
        self.assertEqual(bar.plain(12.0), '  fsck_apfs disk5s1 (image): 0:12, Checking the fsroot tree.')

    def test_a_percentage(self):
        bar = Bar('app_cryptex: sealing', 100, PERCENT, 0.0)
        bar.set(0.0, Update(done=0))
        bar.set(4.0, Update(done=40))
        self.assertEqual(bar.line(4.0, 200), 'app_cryptex: sealing  [##########--------------]  40%  0:06 left')

    def test_bytes_without_a_total(self):
        bar = Bar('seed', None, BYTES, 0.0)
        bar.add(0.0, 0)
        bar.add(2.0, 3 << 20)
        self.assertEqual(bar.plain(2.0), '  seed: 3.0 MiB, 1.5 MiB/s, 0:02')


@mock.patch.dict(os.environ, {'COLUMNS': '80', 'LINES': '24'})
class ScreenTest(unittest.TestCase):
    def setUp(self):
        self.out, self.clock = io.StringIO(), Clock()

    def test_a_terminal_draws_over_one_line_and_logs_above_it(self):
        screen = progress.Screen(self.out, tty=True, now=self.clock)
        bar = Bar('copy', 100, 'files', 0.0)
        screen.push(bar)
        drawn = '\r' + bar.line(0.0, 80) + '\x1b[K'
        self.assertEqual(self.out.getvalue(), drawn)
        screen.println('compose: OS container copied')
        screen.pop(bar)
        cleared = '\r\x1b[K'
        self.assertEqual(self.out.getvalue(), drawn + cleared + 'compose: OS container copied\n' + drawn + cleared)

    def test_a_terminal_redraws_at_most_every_tenth_of_a_second(self):
        screen = progress.Screen(self.out, tty=True, now=self.clock)
        screen.push(Bar('copy', 100, 'files', 0.0))
        before = self.out.getvalue()
        self.clock.now = 0.05
        screen.tick()
        self.assertEqual(self.out.getvalue(), before)
        self.clock.now = 0.1
        screen.tick()
        self.assertNotEqual(self.out.getvalue(), before)

    def test_a_log_gets_a_plain_line_every_five_seconds(self):
        screen = progress.Screen(self.out, tty=False, now=self.clock)
        bar = Bar('copy', 100, 'files', 0.0)
        screen.push(bar)
        self.clock.now = 4.9
        screen.tick()
        self.assertEqual(self.out.getvalue(), '')
        self.clock.now = 5.0
        screen.tick()
        screen.println('done')
        screen.pop(bar)
        self.assertEqual(self.out.getvalue(), bar.plain(5.0) + '\n' + 'done\n')

    def test_a_probe_is_polled(self):
        screen = progress.Screen(self.out, tty=False, now=self.clock)
        bar = Bar('ditto', 100, BYTES, 0.0, probe=lambda: 60)
        screen.push(bar)
        self.clock.now = 1.0
        screen.tick()
        self.assertEqual(bar.done, 60)

    def test_a_probe_that_fails_leaves_the_step_as_it_was(self):
        def gone() -> float:
            raise FileNotFoundError('/tmp/switch-unmounted')

        screen = progress.Screen(self.out, tty=False, now=self.clock)
        bar = Bar('ditto', 100, BYTES, 0.0, probe=gone)
        screen.push(bar)
        self.clock.now = 1.0
        screen.tick()
        self.assertEqual(bar.done, 0)


class StepTest(unittest.TestCase):
    def setUp(self):
        self.out = io.StringIO()
        patcher = mock.patch.object(progress, 'SCREEN', progress.Screen(self.out, tty=False, now=Clock()))
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_nothing_happens_without_a_step(self):
        progress.advance(10)
        progress.note('diskutil eject disk4')
        progress.apply(Update(done=5, total=10))
        progress.tick()
        self.assertEqual(self.out.getvalue(), '')

    def test_the_innermost_step_counts(self):
        with progress.step('populate', unit=None) as outer:
            with progress.step('preboot: staging', 100) as inner:
                progress.advance(30)
                progress.note('copying')
            progress.note('diskutil unmount disk5s3')
        self.assertEqual((inner.done, inner.note), (30, 'copying'))
        self.assertEqual((outer.done, outer.note), (0, 'diskutil unmount disk5s3'))
        self.assertEqual(progress.SCREEN.steps, [])

    def test_println_without_a_step_is_print(self):
        progress.println('image: iPadOS 27.2 (24B119) as IOS27.SII')
        self.assertEqual(self.out.getvalue(), 'image: iPadOS 27.2 (24B119) as IOS27.SII\n')


class ParserTest(unittest.TestCase):
    def test_the_sealer(self):
        line = '** Sealing volume disk38s1: 1435 hash records added, done 68% of the fs blocks'
        self.assertEqual(progress.sealer(line), Update(done=68, total=100, unit=PERCENT))
        self.assertIsNone(progress.sealer('Sealed fsroot tree has 3004 records'))
        self.assertIsNone(
            progress.sealer('Unexpected extended attribute com.apple.provenance for inode 16 (flags 0x2)')
        )

    def test_hdiutil_puppetstrings(self):
        self.assertEqual(progress.puppetstrings('PERCENT:63.529705'), Update(done=63.529705, total=100, unit=PERCENT))
        self.assertEqual(progress.puppetstrings('PERCENT:-1.000000'), Update())
        self.assertEqual(
            progress.puppetstrings('MESSAGE:Checksumming GPT Header (Primary GPT Header : 1)…'),
            Update(note='Checksumming GPT Header (Primary GPT Header : 1)…'),
        )
        self.assertIsNone(progress.puppetstrings('created: /tmp/z.dmg'))

    def test_our_tools(self):
        self.assertEqual(progress.ours('progress 12/3413 files'), Update(done=12, total=3413, unit='files', note=''))
        self.assertEqual(progress.ours('progress 0/9 dyld cache files').unit, 'dyld cache files')
        self.assertEqual(progress.ours('progress 5/10 bytes').unit, BYTES)
        self.assertEqual(progress.ours('progress flushing to the card'), Update(note='flushing to the card'))
        self.assertIsNone(progress.ours('os: 3413 files translated, 3413 trust entries'))

    def test_phases_are_notes_and_output(self):
        self.assertEqual(
            progress.phases('** Checking the fsroot tree.'), Update(note='Checking the fsroot tree.', keep=True)
        )
        self.assertEqual(progress.phases('Verifying storage system').note, 'Verifying storage system')
        self.assertIsNone(progress.phases('   '))


if __name__ == '__main__':
    unittest.main()
