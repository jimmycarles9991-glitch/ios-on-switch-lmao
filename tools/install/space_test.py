"""Measured space: what files take on disk, a delta's translated files, and a refusal that says how much is missing."""

import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools.install import host, space


class Space(unittest.TestCase):
    def setUp(self) -> None:
        self.dir = Path(tempfile.mkdtemp())

    def test_allocated_counts_blocks_not_lengths(self) -> None:
        (self.dir / 'sub').mkdir()
        (self.dir / 'sub/a').write_bytes(b'x' * 10)
        with open(self.dir / 'sparse', 'wb') as f:
            f.truncate(64 << 20)
        self.assertEqual(space.allocated(self.dir / 'sub/a'), (self.dir / 'sub/a').stat().st_blocks * 512)
        self.assertLess(space.allocated(self.dir), 1 << 20)
        self.assertEqual(space.allocated(None), 0)

    def test_a_delta_is_its_translated_files(self) -> None:
        (self.dir / 'files').mkdir()
        (self.dir / 'files/t').write_bytes(b'y' * 9000)
        entry = {'path': '/usr/bin/t', 'file': 'files/t'}
        (self.dir / 'os.manifest.json').write_text(json.dumps({'files': [entry]}))
        self.assertEqual(space.delta_bytes(self.dir), space.allocated(self.dir / 'files/t'))

    def test_a_short_disk_is_refused_with_the_shortfall(self) -> None:
        with mock.patch.object(space.disks, 'free_bytes', return_value=space.KEEP_FREE + (1 << 30)):
            space.check(self.dir, 1 << 30, 'compose')
            with self.assertRaisesRegex(host.HostError, r'compose writes 2.0 GiB .* 1.0 GiB more is needed'):
                space.check(self.dir, 2 << 30, 'compose')

    def test_the_whole_run_is_stated_and_refused_before_it_starts(self) -> None:
        lines = []
        rows = [('compose', 8 << 30), ('populate', 7 << 30), ('pack', 8 << 30)]
        with mock.patch.object(space.disks, 'free_bytes', return_value=20 << 30):
            with self.assertRaisesRegex(host.HostError, r'^pack would run out .* needs 23 GiB .* clear 5.0 GiB'):
                space.check_run(self.dir, rows, 13 << 30, lines.append)
            self.assertIn('this run writes 23 GiB (compose 8.0 GiB, populate 7.0 GiB, pack 8.0 GiB)', lines[0])
            self.assertIn('already take 13 GiB', lines[1])
            space.check_run(self.dir, rows[1:], 0, lines.append)


if __name__ == '__main__':
    unittest.main()
