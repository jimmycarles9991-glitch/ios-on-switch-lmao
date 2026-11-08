"""A delta rewrites files in place (inode and mode kept), resumes, and refuses files it was not made from."""

import hashlib
import json
import os
import tempfile
import unittest
from pathlib import Path

from tools.install import delta


def sha(data: bytes):
    return hashlib.sha256(data).hexdigest()


def quiet(*parts: object):
    pass


class DeltaTest(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.volume = self.dir / 'volume'
        (self.volume / 'usr/libexec').mkdir(parents=True)
        (self.volume / 'private/var/staged').mkdir(parents=True)
        self.tool = self.volume / 'usr/libexec/tool'
        self.tool.write_bytes(b'arm64e')
        self.tool.chmod(0o751)
        (self.volume / 'private/var/staged/app').write_bytes(b'app arm64e')
        out = self.dir / 'delta'
        (out / 'files').mkdir(parents=True)
        (out / 'files/tool').write_bytes(b'arm64 translated')
        (out / 'files/app').write_bytes(b'app arm64')
        entries = [
            {'path': '/usr/libexec/tool', 'file': 'files/tool', 'sha256_in': sha(b'arm64e'),
             'sha256_out': sha(b'arm64 translated')},
            {'path': '/private/var/staged/app', 'file': 'files/app', 'sha256_in': sha(b'app arm64e'),
             'sha256_out': sha(b'app arm64')},
        ]  # fmt: skip
        (out / 'os.manifest.json').write_text(json.dumps({'role': 'os', 'translator': 'test', 'files': entries}))
        self.entries = delta.load(out / 'os.manifest.json')

    def test_rewrites_in_place(self):
        inode = os.stat(self.tool).st_ino
        self.assertEqual(delta.apply(self.volume, self.entries, quiet, 'System'), 2)
        self.assertEqual(self.tool.read_bytes(), b'arm64 translated')
        self.assertEqual(os.stat(self.tool).st_ino, inode)
        self.assertEqual(self.tool.stat().st_mode & 0o777, 0o751)

    def test_consume_deletes_each_replacement_once_written(self):
        delta.apply(self.volume, self.entries, quiet, 'System', consume=True)
        self.assertFalse(any(e['source'].exists() for e in self.entries))
        self.assertEqual(delta.apply(self.volume, self.entries, quiet, 'System'), 0)

    def test_hard_links_are_rewritten_once_and_each_replacement_consumed(self):
        (self.volume / 'usr/sbin').mkdir()
        (self.volume / 'usr/sbin/tool').hardlink_to(self.tool)
        twin = self.entries[0]['source'].with_name('twin')
        twin.write_bytes(self.entries[0]['source'].read_bytes())
        entries = [self.entries[0], dict(self.entries[0], path='/usr/sbin/tool', source=twin)]
        self.assertEqual(delta.apply(self.volume, entries, quiet, 'System', consume=True), 1)
        self.assertEqual((self.volume / 'usr/sbin/tool').read_bytes(), b'arm64 translated')
        self.assertFalse(any(e['source'].exists() for e in entries))

    def test_resumes(self):
        delta.apply(self.volume, self.entries, quiet, 'System')
        self.assertEqual(delta.apply(self.volume, self.entries, quiet, 'System'), 0)

    def test_refuses_another_file(self):
        self.tool.write_bytes(b'something else')
        with self.assertRaisesRegex(delta.DeltaError, 'not the file the delta was made from'):
            delta.apply(self.volume, self.entries, quiet, 'System')

    def test_data_takes_the_entries_below_private_var(self):
        data = self.dir / 'data'
        (data / 'staged').mkdir(parents=True)
        (data / 'staged/app').write_bytes(b'app arm64e')
        below = delta.entries_under(self.entries, '/private/var')
        self.assertEqual([e['path'] for e in below], ['/staged/app'])
        self.assertEqual(delta.apply(data, below, quiet, 'Data'), 1)
        self.assertEqual((data / 'staged/app').read_bytes(), b'app arm64')

    def test_refuses_relative_paths(self):
        manifest = self.dir / 'bad.manifest.json'
        manifest.write_text(
            json.dumps({'files': [{'path': 'usr/../etc', 'file': 'x', 'sha256_in': '', 'sha256_out': ''}]})
        )
        with self.assertRaises(delta.DeltaError):
            delta.load(manifest)


class AddedBytesTest(unittest.TestCase):
    def test_counts_the_replacement_in_whole_blocks_less_what_it_replaces(self):
        root = Path(tempfile.mkdtemp())
        (root / 'bin').mkdir()
        (root / 'bin/tool').write_bytes(b'x' * 100)
        source = root / 'replacement'
        source.write_bytes(b'y' * 9000)
        entries = [{'path': '/bin/tool', 'source': source}]
        old = os.lstat(root / 'bin/tool').st_blocks * 512
        self.assertEqual(delta.added_bytes(root, entries), 3 * 4096 - old)

    def test_a_smaller_replacement_gives_room_back(self):
        root = Path(tempfile.mkdtemp())
        (root / 'big').write_bytes(b'x' * 65536)
        source = root / 'small'
        source.write_bytes(b'y')
        self.assertLess(delta.added_bytes(root, [{'path': '/big', 'source': source}]), 0)


class WrittenBytesTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.a = self.root / 'a'
        self.a.write_bytes(b'x' * 5000)
        self.b = self.root / 'b'
        self.b.write_bytes(b'y')

    def test_counts_every_replacement_whole(self):
        entries = [{'path': '/a', 'source': self.a}, {'path': '/b', 'source': self.b}]
        self.assertEqual(delta.written_bytes(self.root, entries), 2 * 4096 + 4096)

    def test_counts_hard_links_once(self):
        (self.root / 'c').hardlink_to(self.a)
        entries = [{'path': '/a', 'source': self.a}, {'path': '/c', 'source': self.a}]
        self.assertEqual(delta.written_bytes(self.root, entries), 2 * 4096)


if __name__ == '__main__':
    unittest.main()
