"""iOS 27's deltas as Bazel leaves them.

What the boot package keeps of them, and the System files placed on Data from them without consuming them.
"""

import hashlib
import json
import os
import stat
import tempfile
import unittest
from pathlib import Path

from tools.install import host, overlay_index, owners, pretranslate


def fake_delta(out: Path, role: str) -> None:
    """Write what translate_tree writes for one role: one translated file, its manifest, trust module and pairs."""
    (out / 'files').mkdir(parents=True, exist_ok=True)
    (out / 'files' / 'tool').write_bytes(b'arm64')
    entry = {
        'path': '/usr/bin/tool',
        'file': 'files/tool',
        'sha256_in': hashlib.sha256(b'arm64e').hexdigest(),
        'sha256_out': hashlib.sha256(b'arm64').hexdigest(),
        'cdhash': ['00' * 20],
    }
    (out / f'{role}.manifest.json').write_text(json.dumps({'role': role, 'translator': 't', 'files': [entry]}))
    (out / f'{role}.trustcache').write_bytes(b'trust ' + role.encode())
    (out / f'{role}.cdhash-pairs').write_text(role + ' old new\n')
    if role == 'system_cryptex':
        (out / 'a64x-cache-pretranslated.bin').write_bytes(b'cache')


def stats(system: Path) -> pretranslate.SystemStat:
    return lambda path: os.lstat(system / path.lstrip('/'))


class Quiet:
    def __call__(self, *parts: object) -> None:
        pass

    def quiet(self, *parts: object) -> None:
        pass


class DeltasTest(unittest.TestCase):
    def setUp(self) -> None:
        self.dir = Path(tempfile.mkdtemp())
        dirs = {}
        for role in pretranslate.ORDER:
            dirs[role] = self.dir / 'bazel-out' / ('delta_' + role)
            fake_delta(dirs[role], role)
        self.roles = pretranslate.Deltas(dirs, self.dir / 'scratch/pretranslated', Quiet())

    def test_kept_files_are_the_boot_package_pins(self) -> None:
        self.roles.keep()
        names = [p.name for p in self.roles.artifacts()]
        for role in pretranslate.ORDER:
            self.assertIn(role + '.trustcache', names)
            self.assertIn(role + '.cdhash-pairs', names)
        self.assertIn('a64x-cache-pretranslated.bin', names)
        self.assertNotIn('os.manifest.json', names)

    def test_entries_point_into_the_delta(self) -> None:
        entries = self.roles.entries('os')
        self.assertEqual(entries[0]['sha256_out'], hashlib.sha256(b'arm64').hexdigest())
        self.assertEqual(entries[0]['source'].read_bytes(), b'arm64')

    def test_system_files_go_to_the_data_overlay_with_their_modes(self):
        system = self.dir / 'system'
        (system / 'usr/libexec').mkdir(parents=True)
        (system / 'usr/libexec/tool').write_bytes(b'arm64e')
        (system / 'usr/libexec/tool').chmod(0o750)
        entries = self.roles.entries('os')
        data = self.dir / 'data'
        data.mkdir()
        entries[0]['path'] = '/usr/libexec/tool'
        manifest = self.roles.overlay_system(data, stats(system), entries, None)
        placed = data / pretranslate.OVERLAY / 'usr/libexec/tool'
        self.assertEqual(placed.read_bytes(), b'arm64')
        self.assertEqual(placed.stat().st_mode & 0o7777, 0o750)
        self.assertEqual((system / 'usr/libexec/tool').read_bytes(), b'arm64e')
        self.assertTrue(entries[0]['source'].exists(), "Bazel's delta is only read")
        self.assertEqual(manifest['files'][0]['mode'], '0750')
        self.assertEqual(json.loads((data / pretranslate.OVERLAY_MANIFEST).read_text()), manifest)
        files, directories = overlay_index.decode((data / pretranslate.OVERLAY_INDEX).read_bytes())
        ids = ((system / 'usr/libexec/tool').stat().st_ino, placed.stat().st_ino)
        self.assertEqual(files, [('/usr/libexec/tool', *ids, b'\x00' * 20)])
        tree = data / pretranslate.OVERLAY
        self.assertEqual(directories, sorted(p.stat().st_ino for p in (tree, tree / 'usr', tree / 'usr/libexec')))
        self.assertIn('os.overlay.json', [p.name for p in self.roles.artifacts()])

    def test_the_tree_ends_system_immutable_with_the_system_files_owners(self) -> None:
        system = self.dir / 'system'
        (system / 'usr/libexec').mkdir(parents=True)
        (system / 'usr/libexec/tool').write_bytes(b'arm64e')
        entries = self.roles.entries('os')
        entries[0]['path'] = '/usr/libexec/tool'
        data = self.dir / 'data'
        data.mkdir()
        journal = owners.Journal(data)
        self.roles.overlay_system(data, stats(system), entries, journal)
        tree = data / pretranslate.OVERLAY
        expected = {
            tree,
            tree / 'usr',
            tree / 'usr/libexec',
            tree / 'usr/libexec/tool',
            data / pretranslate.OVERLAY_INDEX,
        }
        self.assertEqual({data / k for k in journal.wanted}, expected)
        for name, owner in journal.wanted.items():
            self.assertTrue(owner.flags & stat.SF_IMMUTABLE, name)
        self.assertEqual(journal.wanted[f'{pretranslate.OVERLAY}/usr/libexec/tool'].uid, os.getuid())

    def test_hard_links_share_one_data_file_and_each_get_an_entry(self):
        system = self.dir / 'system'
        (system / 'usr/libexec').mkdir(parents=True)
        (system / 'usr/sbin').mkdir(parents=True)
        (system / 'usr/libexec/tool').write_bytes(b'arm64e')
        (system / 'usr/sbin/tool').hardlink_to(system / 'usr/libexec/tool')
        entries = self.roles.entries('os')
        twin = entries[0]['source'].with_name('twin')
        twin.write_bytes(entries[0]['source'].read_bytes())
        entries = [dict(entries[0], path='/usr/libexec/tool'), dict(entries[0], path='/usr/sbin/tool', source=twin)]
        data = self.dir / 'data'
        data.mkdir()
        manifest = self.roles.overlay_system(data, stats(system), entries, None)
        tree = data / pretranslate.OVERLAY
        self.assertFalse((tree / 'usr/sbin').exists())
        self.assertTrue(twin.exists())
        self.assertEqual([f['path'] for f in manifest['files']], ['/usr/libexec/tool', '/usr/sbin/tool'])
        files, _ = overlay_index.decode((data / pretranslate.OVERLAY_INDEX).read_bytes())
        ids = ((system / 'usr/libexec/tool').stat().st_ino, (tree / 'usr/libexec/tool').stat().st_ino)
        self.assertEqual([f[1:3] for f in files], [ids, ids])

    def staged(self, pairs: str) -> tuple[Path, Path, list[dict]]:
        system = self.dir / 'system'
        app = system / 'private/var/staged_system_apps/FaceTime.app'
        app.mkdir(parents=True)
        (app / 'FaceTime').write_bytes(b'arm64e')
        (app / 'FaceTime').chmod(0o755)
        (self.roles.dirs['os'] / 'os.cdhash-pairs').write_text(pairs)
        entries = [dict(self.roles.entries('os')[0], path='/private/var/staged_system_apps/FaceTime.app/FaceTime')]
        entries[0]['cdhash'] = ['22' * 20]
        data = self.dir / 'data'
        data.mkdir()
        return system, data, entries

    def test_staged_apps_go_to_the_tree_as_roots_with_their_originals(self):
        system, data, entries = self.staged(f'{"11" * 20} {"22" * 20}\n{"33" * 20} {"44" * 20}\n')
        journal = owners.Journal(data)
        self.roles.overlay_system(data, stats(system), entries, journal)
        placed = f'{pretranslate.OVERLAY}/private/var/staged_system_apps/FaceTime.app/FaceTime'
        self.assertEqual((data / placed).read_bytes(), b'arm64')
        self.assertEqual((journal.wanted[placed].uid, journal.wanted[placed].gid), (0, 0))
        self.assertEqual(journal.wanted[placed].mode, 0o755)
        raw = (data / pretranslate.OVERLAY_ORIGINALS).read_bytes()
        self.assertEqual(raw, overlay_index.encode_originals([(bytes([0x11] * 20), bytes([0x22] * 20))]))
        self.assertTrue(journal.wanted[pretranslate.OVERLAY_ORIGINALS].flags & stat.SF_IMMUTABLE)
        files, _ = overlay_index.decode((data / pretranslate.OVERLAY_INDEX).read_bytes())
        self.assertEqual([(f[0], f[3]) for f in files], [(entries[0]['path'], bytes([0x22] * 20))])

    def test_a_staged_translation_without_its_original_is_refused(self):
        system, data, entries = self.staged(f'{"33" * 20} {"44" * 20}\n')
        with self.assertRaisesRegex(host.HostError, 'without an original'):
            self.roles.overlay_system(data, stats(system), entries, None)

    def test_without_staged_apps_there_are_no_originals(self):
        system = self.dir / 'system'
        (system / 'usr/bin').mkdir(parents=True)
        (system / 'usr/bin/tool').write_bytes(b'arm64e')
        data = self.dir / 'data'
        data.mkdir()
        self.roles.overlay_system(data, stats(system), self.roles.entries('os'), None)
        self.assertFalse((data / pretranslate.OVERLAY_ORIGINALS).exists())

    def test_a_symlink_is_refused(self):
        system = self.dir / 'system'
        (system / 'usr/bin').mkdir(parents=True)
        (system / 'usr/bin/real').write_bytes(b'arm64e')
        (system / 'usr/bin/tool').symlink_to('real')
        entries = self.roles.entries('os')
        with self.assertRaisesRegex(host.HostError, 'not a regular file'):
            self.roles.overlay_system(self.dir / 'data', stats(system), entries, None)

    def test_a_translated_file_that_does_not_match_is_refused(self):
        system = self.dir / 'system'
        (system / 'usr/bin').mkdir(parents=True)
        (system / 'usr/bin/tool').write_bytes(b'arm64e')
        entries = self.roles.entries('os')
        entries[0]['source'].write_bytes(b'corrupt')
        with self.assertRaisesRegex(host.HostError, 'does not match'):
            self.roles.overlay_system(self.dir / 'data', stats(system), entries, None)

    def test_pins_refuse_another_translation(self) -> None:
        self.roles.keep()
        digest = hashlib.sha256(b'trust os').hexdigest()
        self.roles.check_pins({'os.trustcache': digest})
        with self.assertRaisesRegex(host.HostError, 'os.trustcache'):
            self.roles.check_pins({'os.trustcache': '0' * 64})
        with self.assertRaisesRegex(host.HostError, 'missing.bin'):
            self.roles.check_pins({'missing.bin': digest})

    def test_unknown_roles_refused(self):
        with self.assertRaises(host.HostError):
            pretranslate.Deltas({'kernel': self.dir}, self.dir, Quiet())


if __name__ == '__main__':
    unittest.main()
