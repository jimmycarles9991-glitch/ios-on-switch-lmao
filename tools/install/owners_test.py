"""The owner journal on a plain directory: named paths, inherited owners, copied trees, hard links, round trips."""

import os
import tempfile
import unittest
from pathlib import Path

from tools.install import owners


class Journal(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(tempfile.mkdtemp(prefix='journal-'))
        (self.root / 'a/b').mkdir(parents=True)
        (self.root / 'a/b/file').write_text('x')
        (self.root / 'c').write_text('y')

    def tearDown(self) -> None:
        for path in sorted(self.root.rglob('*'), reverse=True):
            path.rmdir() if path.is_dir() and not path.is_symlink() else path.unlink()
        self.root.rmdir()

    def ino(self, relative: str) -> int:
        return os.lstat(self.root / relative).st_ino

    def test_unnamed_paths_take_their_directory_owner(self) -> None:
        journal = owners.Journal()
        journal.set('', 0, 0, 0o755)
        journal.set('/a/b/', 501, 501, 0o700)
        by_inode = journal.resolve(self.root)
        self.assertEqual(by_inode[self.ino('a')], owners.Owner(0, 0))
        self.assertEqual(by_inode[self.ino('a/b')], owners.Owner(501, 501, 0o700))
        self.assertEqual(by_inode[self.ino('a/b/file')], owners.Owner(501, 501))
        self.assertEqual(len(by_inode), 5)

    def test_the_root_needs_an_owner(self) -> None:
        with self.assertRaises(owners.OwnersError):
            owners.Journal().resolve(self.root)

    def test_one_inode_with_two_owners_is_refused(self) -> None:
        os.link(self.root / 'c', self.root / 'a/c')
        journal = owners.Journal()
        journal.set('', 0, 0)
        journal.set('a', 501, 501)
        with self.assertRaises(owners.OwnersError):
            journal.resolve(self.root)

    def test_copy_records_a_tree_where_it_lands(self) -> None:
        journal = owners.Journal()
        self.assertEqual(journal.copy(self.root / 'a', 'var'), 3)
        mine = os.getuid()
        self.assertEqual(journal.wanted['var/b/file'].uid, mine)
        self.assertEqual(journal.wanted['var'].mode, (self.root / 'a').stat().st_mode & 0o7777)

    def test_save_and_load(self) -> None:
        journal = owners.Journal()
        journal.set('x/y', 0, 80, 0o640, 0x20000)
        saved = self.root / 'journal.json'
        journal.save(saved)
        self.assertEqual(owners.Journal.load(saved).wanted, journal.wanted)


if __name__ == '__main__':
    unittest.main()
