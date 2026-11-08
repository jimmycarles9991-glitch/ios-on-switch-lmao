"""Tools run as before while the running step shows what they print: progress lines read, the rest returned."""

import io
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools.install import host, progress


class StepTest(unittest.TestCase):
    def setUp(self):
        patcher = mock.patch.object(progress, 'SCREEN', progress.Screen(io.StringIO(), tty=False))
        patcher.start()
        self.addCleanup(patcher.stop)


class RunTest(StepTest):
    def test_output_and_failure_as_before(self):
        self.assertEqual(host.run('/bin/echo', 'hello'), 'hello\n')
        self.assertEqual(host.run('/bin/echo', 'hello', binary=True), b'hello\n')
        with self.assertRaisesRegex(host.HostError, r'^/bin/sh -c echo broken >&2; exit 3 failed \(3\): broken$'):
            host.run('/bin/sh', '-c', 'echo broken >&2; exit 3')

    def test_the_command_is_the_note_of_a_step_that_counts_nothing(self):
        with progress.step('compose', unit=None) as bar:
            host.run('/bin/echo', '-n', '/dev/disk4')
            with progress.step('ditto', 100) as counted:
                host.run('/bin/echo', 'copying')
        self.assertEqual((bar.note, counted.note), ('echo disk4', None))


class StreamTest(StepTest):
    def test_progress_lines_move_the_step_and_the_rest_is_output(self):
        script = (
            "printf 'progress 1/4 files\\nreal output\\n'; printf 'progress 2/4 files\\rprogress 3/4 files\\r'; "
            "echo err >&2; printf 'tail'"
        )
        with progress.step('os: translating', unit='files') as bar:
            result = host.stream(['/bin/sh', '-c', script], progress.ours)
        self.assertEqual((result.returncode, result.stdout, result.stderr), (0, 'real output\ntail\n', 'err\n'))
        self.assertEqual((bar.done, bar.total, bar.unit), (3, 4, 'files'))

    def test_phases_stay_in_the_output(self):
        script = "printf '** Checking the fsroot tree.\\n** The container /dev/rdisk34 appears to be OK.\\n'"
        with progress.step('fsck_apfs', unit=None) as bar:
            result = host.stream(['/bin/sh', '-c', script], progress.phases)
        self.assertEqual(
            result.stdout, '** Checking the fsroot tree.\n** The container /dev/rdisk34 appears to be OK.\n'
        )
        self.assertEqual(bar.note, 'The container /dev/rdisk34 appears to be OK.')

    def test_watch_fails_as_run_does(self):
        self.assertEqual(host.stream(['/bin/sh', '-c', 'exit 65'], progress.sealer).returncode, 65)
        script = (
            "echo '** Sealing volume disk5s1: 2 hash records added, done 9% of the fs blocks'; echo bad >&2; exit 65"
        )
        with self.assertRaisesRegex(host.HostError, r'failed \(65\): bad$'):
            host.watch('/bin/sh', '-c', script, parse=progress.sealer)

    def test_a_line_cut_between_reads_is_one_line(self):
        lines = host._Lines(progress.ours)
        for data in (b'progress 1/2 fi', b'les\r', b'\nnext\r', b'\n'):
            lines.feed(data)
        self.assertEqual(lines.text(), 'next\n')


class CopyTest(StepTest):
    def test_each_piece_counts_toward_the_step(self):
        size = 2 * host.CHUNK + 4096
        with tempfile.TemporaryDirectory() as directory:
            source, target = Path(directory) / 'source', Path(directory) / 'target'
            source.write_bytes(os.urandom(4096) * (size // 4096))
            with progress.step('copy', size) as bar:
                host.copy_file_parallel(source, target)
            self.assertEqual(target.read_bytes(), source.read_bytes())
        self.assertEqual(bar.done, size)


if __name__ == '__main__':
    unittest.main()
