import os
import stat
import tempfile
import unittest
from pathlib import Path

from tools.common import files


class Replacing(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name)
        self.target = self.root / 'ios.ini'
        self.target.write_text('old')

    def tearDown(self):
        self.dir.cleanup()

    def test_the_partial_keeps_the_name_in_front(self):
        self.assertEqual(files.partial(self.target), self.root / 'ios.ini.partial')

    def test_a_finished_write_replaces_the_file(self):
        with files.replacing(self.target, sync=True) as part:
            self.assertEqual(part, files.partial(self.target))
            part.write_text('new')
            self.assertEqual(self.target.read_text(), 'old')
        self.assertEqual(self.target.read_text(), 'new')
        self.assertEqual(sorted(p.name for p in self.root.iterdir()), ['ios.ini'])

    def test_a_failed_write_leaves_the_file_and_no_partial(self):
        with self.assertRaises(KeyboardInterrupt), files.replacing(self.target) as part:
            part.write_text('half')
            raise KeyboardInterrupt
        self.assertEqual(self.target.read_text(), 'old')
        self.assertEqual(sorted(p.name for p in self.root.iterdir()), ['ios.ini'])

    def test_an_earlier_runs_partial_is_not_taken_for_this_one(self):
        files.partial(self.target).write_text('stale')
        with files.replacing(self.target) as part:
            self.assertFalse(part.exists())
            part.write_text('new')
        self.assertEqual(self.target.read_text(), 'new')

    def test_write_sets_the_mode(self):
        files.write_bytes(self.target, b'\1\2', mode=0o600)
        self.assertEqual(self.target.read_bytes(), b'\1\2')
        self.assertEqual(stat.S_IMODE(self.target.stat().st_mode), 0o600)
        files.write_text(self.root / 'new.json', '{}\n')
        self.assertEqual((self.root / 'new.json').read_text(), '{}\n')

    def test_copy_keeps_times_only_when_asked(self):
        source = self.root / 'source'
        source.write_bytes(b'data')
        os.utime(source, (1_000_000, 1_000_000))
        files.copy(source, self.root / 'plain')
        files.copy(source, self.root / 'timed', keep_times=True)
        self.assertEqual((self.root / 'plain').read_bytes(), b'data')
        self.assertNotEqual((self.root / 'plain').stat().st_mtime, 1_000_000)
        self.assertEqual((self.root / 'timed').stat().st_mtime, 1_000_000)


if __name__ == '__main__':
    unittest.main()
