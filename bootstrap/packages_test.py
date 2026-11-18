import gzip
import io
import tempfile
import unittest
from pathlib import Path

from bootstrap import packages


class PackageTests(unittest.TestCase):
    def test_timezone_link_is_exactly_scoped(self):
        for name, target in (
            ('var/jb/etc/localtime', '/var/db/timezone/localtime'),
            ('var/jb/etc/localtime', '/private/etc/passwd'),
            ('var/jb/etc/other', '/var/db/timezone/localtime'),
        ):
            entries = {}
            packages.add_link(entries, name, target)
            with packages.tarfile.open(fileobj=io.BytesIO(packages.tar_bytes(entries))) as archive:
                if name == 'var/jb/etc/localtime' and target == '/var/db/timezone/localtime':
                    decoded = packages.read_tar(archive)
                    self.assertEqual(decoded['etc/localtime'][0].linkname, target)
                else:
                    with self.assertRaises(ValueError):
                        packages.read_tar(archive)

    def test_debian_round_trip_is_reproducible(self):
        control, files = {}, {}
        packages.add_file(control, 'control', b'Package: demo\nArchitecture: iphoneos-arm64\nVersion: 1\n')
        packages.add_file(files, 'var/jb/usr/bin/demo', b'command\n', 0o755)
        with tempfile.TemporaryDirectory() as directory:
            first, second = Path(directory) / 'first.deb', Path(directory) / 'second.deb'
            packages.write_deb(first, control, files)
            packages.write_deb(second, control, files)
            self.assertEqual(first.read_bytes(), second.read_bytes())
            members = packages.read_deb(first)
            self.assertEqual(members['debian-binary'], b'2.0\n')
            with packages.tarfile.open(fileobj=io.BytesIO(members['data.tar.gz'])) as archive:
                decoded = packages.read_tar(archive)
            self.assertEqual(decoded['usr/bin/demo'][1], b'command\n')

    def test_debian_long_paths_use_gnu_headers_while_data_seed_uses_pax(self):
        def record_types(raw: bytes) -> list[bytes]:
            types = []
            offset = 0
            while raw[offset : offset + 512].strip(b'\0'):
                types.append(raw[offset + 156 : offset + 157])
                size = int(raw[offset + 124 : offset + 136].rstrip(b'\0 '), 8)
                offset += 512 * (1 + (size + 511) // 512)
            return types

        control, files = {}, {}
        relative = 'Applications/Demo.app/assets/' + 'long-name-' * 10 + 'image.png'
        long_control = 'control-' + 'long-name-' * 10
        packages.add_file(control, 'control', b'Package: demo\nArchitecture: iphoneos-arm64\nVersion: 1\n')
        packages.add_file(control, long_control, b'metadata\n')
        packages.add_file(files, 'var/jb/' + relative, b'asset\n')
        with tempfile.TemporaryDirectory() as directory:
            deb = Path(directory) / 'demo.deb'
            packages.write_deb(deb, control, files)
            members = packages.read_deb(deb)
        for name in ('control.tar.gz', 'data.tar.gz'):
            types = record_types(gzip.decompress(members[name]))
            self.assertIn(b'L', types)
            self.assertNotIn(b'x', types)
            self.assertNotIn(b'g', types)
        with packages.tarfile.open(fileobj=io.BytesIO(members['control.tar.gz'])) as archive:
            self.assertEqual(packages.read_tar(archive, strip_prefix=False)[long_control][1], b'metadata\n')
        with packages.tarfile.open(fileobj=io.BytesIO(members['data.tar.gz'])) as archive:
            self.assertEqual(packages.read_tar(archive)[relative][1], b'asset\n')
        self.assertIn(b'x', record_types(packages.tar_bytes(files)))

    def test_truncated_debian_archive_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'broken.deb'
            path.write_bytes(b'!<arch>\nshort')
            with self.assertRaises(ValueError):
                packages.read_deb(path)

    def test_paths_and_symlink_ancestors_are_rejected(self):
        for reverse in (False, True):
            entries = {}
            packages.add_link(entries, 'var/jb/escape', 'usr')
            packages.add_file(entries, 'var/jb/escape/bin/command', b'data')
            members = list(entries.values())
            if reverse:
                members.reverse()
            stream = io.BytesIO()
            with packages.tarfile.open(fileobj=stream, mode='w') as archive:
                for member, data in members:
                    archive.addfile(member, io.BytesIO(data) if member.isfile() else None)
            stream.seek(0)
            with (
                packages.tarfile.open(fileobj=stream) as archive,
                self.assertRaises(ValueError),
            ):
                packages.read_tar(archive)
        entries = {}
        packages.add_file(entries, 'var/jb/../../escape', b'data')
        with (
            packages.tarfile.open(fileobj=io.BytesIO(packages.tar_bytes(entries))) as archive,
            self.assertRaises(ValueError),
        ):
            packages.read_tar(archive)

    def test_hardlinks_are_materialized_and_package_is_held(self):
        files = {}
        packages.add_file(files, 'var/jb/usr/bin/first', b'command', 0o755)
        member = packages.tarfile.TarInfo('var/jb/usr/bin/second')
        member.type, member.linkname = packages.tarfile.LNKTYPE, 'var/jb/usr/bin/first'
        files[member.name] = member, b''
        with packages.tarfile.open(fileobj=io.BytesIO(packages.tar_bytes(files))) as archive:
            decoded = packages.read_tar(archive)
        self.assertTrue(decoded['usr/bin/second'][0].isfile())
        self.assertEqual(decoded['usr/bin/second'][1], b'command')
        entries, control = {}, {}
        packages.add_file(entries, 'Library/dpkg/status', b'')
        packages.add_file(control, 'control', b'Package: demo\nVersion: 1\nArchitecture: iphoneos-arm64\n')
        packages.register_package(entries, control, files)
        self.assertIn(b'Status: hold ok installed', entries['Library/dpkg/status'][1])
        self.assertEqual(
            entries['Library/dpkg/info/demo.list'][1],
            b'/var/jb/usr\n/var/jb/usr/bin\n/var/jb/usr/bin/first\n/var/jb/usr/bin/second\n',
        )


if __name__ == '__main__':
    unittest.main()
