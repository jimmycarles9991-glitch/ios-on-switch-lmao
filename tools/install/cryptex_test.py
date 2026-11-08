"""cryptex.rebuild as the user, on a stand-in for Apple's cryptex.

The source is a sealed volume with root's and admin's files, a compressed file and a hard link, as a decrypted cryptex
is. Its delta replaces the compressed file with a larger one and the hard-linked file through one of its two paths.
The rebuild must hold the replacements (both links), keep every other file compressed and every owner, be sealed,
pass fsck_apfs, and be about the size of what it holds. hdiutil and
diskutil: macOS, outside the sandbox (--strategy=TestRunner=local). The argument is apfs_tool.
"""

import hashlib
import json
import os
import shutil
import stat
import sys
import tempfile
import unittest
import uuid
from pathlib import Path

from tools.common import files
from tools.install import cryptex, delta, disks, host, owners

MIB = 1 << 20
APFS_TOOL = ''


class Log:
    def __call__(self, *parts: object) -> None:
        print(*parts)

    def quiet(self, *parts: object) -> None:
        pass


LOG = Log()


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class Rebuild(unittest.TestCase):
    def setUp(self) -> None:
        self.dir = Path(tempfile.mkdtemp(prefix='cryptex-'))
        self.source = self.dir / 'source.dmg'
        self.delta = self.dir / 'delta'
        self.out = self.dir / 'out' / 'source.dmg'
        self.out.parent.mkdir()

    def tearDown(self) -> None:
        disks.sweep(self.dir, LOG)
        shutil.rmtree(self.dir)

    def make_source(self) -> None:
        with open(self.source, 'wb') as f:
            f.truncate(24 * MIB)
        with disks.attached(self.source, LOG, raw=True) as image:
            host.run('newfs_apfs', '-e', '-v', 'StandIn', '/dev/' + image.whole, log=LOG)
        plain = self.dir / 'plain'
        (plain / 'usr/lib').mkdir(parents=True)
        (plain / 'usr/lib/libbig.dylib').write_bytes(b'\0\1' * MIB)
        (plain / 'usr/lib/libsmall.dylib').write_bytes(os.urandom(64 << 10))
        (plain / 'usr/share').mkdir()
        (plain / 'usr/share/admin.plist').write_bytes(os.urandom(512) * 600)
        journal = owners.Journal()
        journal.set('', 0, 0, 0o755)
        journal.set('usr/share/admin.plist', 0, 80, 0o644)
        with disks.attached(self.source, LOG, raw=True) as image, disks.mounted(image.volumes[0], LOG, False) as root:
            host.run('ditto', '--hfsCompression', plain, root, log=LOG)
            os.link(root / 'usr/lib/libsmall.dylib', root / 'usr/lib/libsame.dylib')
            self.assertTrue(os.lstat(root / 'usr/lib/libbig.dylib').st_flags & delta.COMPRESSED)
            disks.settle(root)
            by_inode = journal.resolve(root)
        fd = os.open(self.source, os.O_RDWR)
        try:
            owners.apply(fd, 0, None, by_inode)
        finally:
            os.close(fd)
        with disks.attached(self.source, LOG, raw=True) as image:
            host.run(cryptex.SEALER, '/dev/' + image.volumes[0], log=LOG)
        shutil.rmtree(plain)

    def make_delta(self) -> dict[str, bytes]:
        replacements = {'/usr/lib/libbig.dylib': os.urandom(3 * MIB), '/usr/lib/libsmall.dylib': os.urandom(80 << 10)}
        originals = {'/usr/lib/libbig.dylib': b'\0\1' * MIB}
        with (
            disks.attached(self.source, LOG, writable=False) as image,
            disks.mounted(image.volumes[0], LOG, owners=False, read_only=True) as root,
        ):
            originals['/usr/lib/libsmall.dylib'] = (root / 'usr/lib/libsmall.dylib').read_bytes()
        self.delta.mkdir()
        files = []
        for i, (path, data) in enumerate(replacements.items()):
            (self.delta / f'f{i}').write_bytes(data)
            files.append({'path': path, 'file': f'f{i}', 'sha256_in': sha(originals[path]), 'sha256_out': sha(data)})
        (self.delta / 'm.json').write_text(json.dumps({'files': files}))
        return replacements

    def test_rebuild(self) -> None:
        self.make_source()
        replacements = self.make_delta()
        entries = delta.load(self.delta / 'm.json')
        volume_uuid = uuid.UUID('ADB8FD34-DA7E-4538-9AAF-4F05EBB09057')
        cryptex.rebuild(cryptex.Job(str(self.source), entries, self.out, APFS_TOOL, 'stand-in', volume_uuid), LOG)
        self.assertFalse(files.partial(self.out).exists())
        self.assertTrue(all(e['source'].is_file() for e in entries), 'the delta is a Bazel input: never consumed')
        self.assertGreater(self.out.stat().st_size, 0)
        with disks.attached(self.out, LOG, writable=False) as image:
            info = host.disk_info(image.volumes[0]) or {}
            self.assertEqual(info.get('VolumeName'), 'StandIn')
            self.assertTrue(info.get('Sealed'))
            self.assertEqual(info.get('VolumeUUID'), str(volume_uuid).upper())
            with disks.mounted(image.volumes[0], LOG, owners=True, read_only=True) as root:
                for path, data in replacements.items():
                    self.assertEqual((root / path.lstrip('/')).read_bytes(), data)
                small, same = os.lstat(root / 'usr/lib/libsmall.dylib'), os.lstat(root / 'usr/lib/libsame.dylib')
                self.assertEqual(small.st_ino, same.st_ino)
                self.assertEqual(stat.S_IMODE(small.st_mode), 0o644)
                self.assertTrue(os.lstat(root / 'usr/share/admin.plist').st_flags & delta.COMPRESSED)
                self.assertFalse(os.lstat(root / 'usr/lib/libbig.dylib').st_flags & delta.COMPRESSED)
                admin = os.lstat(root / 'usr/share/admin.plist')
                self.assertEqual((admin.st_uid, admin.st_gid, stat.S_IMODE(admin.st_mode)), (0, 80, 0o644))
                self.assertEqual(os.lstat(root / 'usr/lib').st_uid, 0)
                used = (os.statvfs(root).f_blocks - os.statvfs(root).f_bfree) * 4096
        self.assertLess(self.out.stat().st_size - used, 3 * MIB, 'the cryptex is about the size of what it holds')


if __name__ == '__main__':
    APFS_TOOL = os.path.abspath(sys.argv.pop(1))
    unittest.main()
