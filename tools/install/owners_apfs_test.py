"""Owners given offline on real APFS, as the user.

A source volume with root's, admin's and mobile's files is copied into a fresh volume, which gets the source's owners
and modes and a system-immutable flag in its inodes; fsck_apfs passes it, a mount that honours owners reads them back,
and they survive apfs_prepare_cryptex's seal. hdiutil and diskutil: macOS, outside the sandbox
(--strategy=TestRunner=local).
"""

import os
import stat
import tempfile
import unittest
from pathlib import Path

from tools.install import apfs, apfs_tree, compose, cryptex, disks, host, owners

MIB = 1 << 20


class Log:
    def __call__(self, *parts: object) -> None:
        print(*parts)

    def quiet(self, *parts: object) -> None:
        pass


LOG = Log()


def blank(path: Path, name: str) -> None:
    with open(path, 'wb') as f:
        f.truncate(32 * MIB)
    with disks.attached(path, LOG, raw=True) as image:
        host.run('newfs_apfs', '-e', '-v', name, '/dev/' + image.whole, log=LOG)


class OwnersOnApfs(unittest.TestCase):
    def setUp(self) -> None:
        self.dir = Path(tempfile.mkdtemp(prefix='owners-'))
        self.source, self.copy = self.dir / 'source.img', self.dir / 'copy.img'
        blank(self.source, 'Source')
        blank(self.copy, 'Copy')

    def tearDown(self) -> None:
        disks.sweep(self.dir, LOG)
        for image in (self.source, self.copy):
            image.unlink(missing_ok=True)
        self.dir.rmdir()

    def fill_source(self) -> None:
        """Files of three owners, a setuid binary, a hard link and a symlink, given offline."""
        journal = owners.Journal()
        with disks.attached(self.source, LOG, raw=True) as image, disks.mounted(image.volumes[0], LOG, False) as root:
            (root / 'usr/bin').mkdir(parents=True)
            (root / 'usr/bin/tool').write_bytes(b'\xcf\xfa\xed\xfe' * 1024)
            (root / 'usr/bin/tool').chmod(0o4755)
            os.link(root / 'usr/bin/tool', root / 'usr/bin/same')
            (root / 'var/mobile').mkdir(parents=True)
            (root / 'var/mobile/prefs.plist').write_text('<plist/>')
            (root / 'var/admin').mkdir()
            (root / 'var/admin/secret').write_text('admin only')
            (root / 'var/admin/secret').chmod(0o600)
            (root / 'var/link').symlink_to('mobile/prefs.plist')
            (root / 'frozen').write_text('do not change')
            journal.set('', 0, 0, 0o755)
            journal.set('var/mobile', 501, 501, 0o755)
            journal.set('var/admin', 0, 80, 0o750)
            journal.set('var/admin/secret', 0, 80)
            journal.set('frozen', 0, 0, 0o444)
            disks.settle(root)
            by_inode = journal.resolve(root)
        fd = os.open(self.source, os.O_RDWR)
        try:
            self.assertGreaterEqual(owners.apply(fd, 0, None, by_inode), len(by_inode))
        finally:
            os.close(fd)

    def test_copy_takes_the_source_owners_and_keeps_them_sealed(self) -> None:
        self.fill_source()
        journal = owners.Journal()
        with disks.attached(self.source, LOG, writable=False, raw=True) as source:
            with disks.mounted(source.volumes[0], LOG, owners=True, read_only=True) as src:
                self.assertEqual((src / 'var/admin/secret').stat().st_gid, 80)
                journal.copy(src)
            journal.set('frozen', 0, 0, 0o444, stat.SF_IMMUTABLE)
            with (
                disks.mounted(source.volumes[0], LOG, owners=False, read_only=True) as src,
                disks.attached(self.copy, LOG, raw=True) as target,
                disks.mounted(target.volumes[0], LOG, owners=False) as root,
            ):
                host.run('ditto', '--preserveHFSCompression', src, root, log=LOG)
                disks.settle(root)
                by_inode = journal.resolve(root)
        fd = os.open(self.copy, os.O_RDWR)
        try:
            owners.apply(fd, 0, None, by_inode)
        finally:
            os.close(fd)
        with disks.attached(self.copy, LOG, raw=True) as target:
            compose.fsck(target.volumes[0].rpartition('s')[0], 'owners given offline', LOG)
            with disks.mounted(target.volumes[0], LOG, owners=True, read_only=True) as root:
                owners.check(root, by_inode)
                self.assertEqual(stat.S_IMODE((root / 'usr/bin/same').stat().st_mode), 0o4755)
                self.assertTrue((root / 'frozen').stat().st_flags & stat.SF_IMMUTABLE)
                self.assertEqual((root / 'var/mobile/prefs.plist').stat().st_uid, 501)
            host.run(cryptex.SEALER, '/dev/' + target.volumes[0], log=LOG)
            with disks.mounted(target.volumes[0], LOG, owners=True, read_only=True) as root:
                owners.check(root, by_inode)
        fd = os.open(self.copy, os.O_RDONLY)
        try:
            volume = apfs_tree.Volume(fd)
            self.assertTrue(volume.sealed)
            with self.assertRaises(apfs.ApfsError):
                volume.rewrite({2: apfs_tree.Inode(0, 0, 0o755, 0)})
        finally:
            os.close(fd)

    def test_an_inode_without_a_path_is_refused(self) -> None:
        with disks.attached(self.copy, LOG, raw=True) as image, disks.mounted(image.volumes[0], LOG, False) as root:
            (root / 'kept').write_text('x')
            journal = owners.Journal()
            journal.set('', 0, 0)
            disks.settle(root)
            by_inode = journal.resolve(root)
            (root / 'late').write_text('written after the walk')
        fd = os.open(self.copy, os.O_RDWR)
        try:
            with self.assertRaises(owners.OwnersError):
                owners.apply(fd, 0, None, by_inode)
        finally:
            os.close(fd)


if __name__ == '__main__':
    unittest.main()
