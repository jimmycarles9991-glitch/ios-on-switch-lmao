import hashlib
import io
import json
import os
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools.install import artifacts, bootstrap, bootstrap_migration, host, owners

PINS = {'build': '21H461', 'version': '17.7.11'}
STATUS = (
    'Package: org.coolstar.sileo\nStatus: install ok installed\nVersion: 2.5.1\n\n'
    'Package: ellekit\nStatus: install ok installed\nVersion: 1.1\n\n'
    'Package: uikittools\nStatus: install ok installed\nVersion: 2.1\n\n'
    'Package: launchctl\nStatus: install ok installed\nVersion: 1:1.1.1\n\n'
    'Package: firmware\nStatus: install ok installed\nVersion: 15.0\n'
)


def regular(name: str, data: bytes = b'x', mode: int = 0o644, uid: int = 0, gid: int = 0):
    info = tarfile.TarInfo(name)
    info.size, info.mode, info.uid, info.gid = len(data), mode, uid, gid
    return info, data


def link(name: str, target: str, hard: bool = False):
    info = tarfile.TarInfo(name)
    info.type, info.linkname = (tarfile.LNKTYPE if hard else tarfile.SYMTYPE), target
    info.mode = 0o755 if hard else 0o777
    return info, b''


def package(root: Path, extra: tuple | list = (), helper: bytes = b'helper') -> Path:
    directory = root / 'bootstrap'
    directory.mkdir(parents=True, exist_ok=True)
    with tarfile.open(directory / bootstrap.ARCHIVE, 'w') as archive:
        for info, data in [
            regular('.procursus_strapped', b''),
            regular('.switch-bootstrap-ready', b''),
            regular('Library/dpkg/status', STATUS.encode()),
            regular('usr/bin/tool', b'tool', 0o755),
            *extra,
        ]:
            archive.addfile(info, io.BytesIO(data) if info.isfile() else None)
    executable = root / 'overlay/usr/libexec' / bootstrap.HELPER
    executable.parent.mkdir(parents=True, exist_ok=True)
    executable.write_bytes(helper)
    value = {
        'schema_version': 1,
        'archive_sha256': hashlib.sha256((directory / bootstrap.ARCHIVE).read_bytes()).hexdigest(),
        'helper_sha256': hashlib.sha256(helper).hexdigest(),
        'holds': list(bootstrap.HELD),
    }
    (directory / bootstrap.MANIFEST).write_text(json.dumps(value))
    return root


class BootstrapTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.data = self.base / 'data'
        self.data.mkdir()
        self.package = package(self.base / 'package')
        self.log = []

    def stage(self, pins: dict = PINS, journal: owners.Owners | None = None):
        return bootstrap.stage(self.data, self.package, pins, self.log.append, journal)

    def installed(self, build: str = '21H461'):
        return self.data / bootstrap.DIRECTORY / build

    def test_both_builds_use_their_pinned_firmware_and_distinct_package_state(self):
        for build, version in (('21H461', '17.7.11'), ('24B119', '27.2')):
            pins = {'build': build, 'version': version}
            self.assertTrue(self.stage(pins))
            installed = self.installed(build)
            state = json.loads((installed / bootstrap.MANIFEST).read_text())
            self.assertEqual((state['build'], state['firmware']), (build, version))
            status = bootstrap._control((installed / 'root/Library/dpkg/status').read_text())
            records = {item['Package']: item for item in status}
            self.assertEqual(records['firmware']['Version'], version)
            self.assertTrue(all(records[name]['Status'] == 'hold ok installed' for name in bootstrap.HELD))
            self.assertEqual(records['uikittools']['Status'], 'hold ok installed')
            self.assertEqual(os.readlink(self.data / 'jb'), f'/private/var/Switch/bootstrap/{build}/root')
        self.assertTrue((self.installed() / 'root/usr/bin/tool').is_file())

    def test_fresh_image_and_redeploy_schedule_each_generation_once(self):
        manifest = self.package / 'bootstrap' / bootstrap.MANIFEST
        value = json.loads(manifest.read_text())
        first, second = 'b' * 64, 'c' * 64
        value['managed_generation'] = first
        manifest.write_text(json.dumps(value))
        journal = owners.Journal(self.data)
        self.assertTrue(self.stage(journal=journal))
        base = self.installed()
        pending = base / bootstrap_migration.PENDING
        receipt = base / bootstrap_migration.RECEIPT
        guard = self.data / 'Switch/system-app-migration'
        sentinel = self.data / 'datamigrator/migrate'
        self.assertEqual(pending.read_bytes(), (first + '\n').encode())
        self.assertEqual(journal.wanted['Switch/bootstrap/21H461/migration.pending'], owners.Owner(0, 0, 0o644))
        self.assertFalse(guard.exists())
        guard.touch()
        self.assertFalse(self.stage())
        self.assertTrue(guard.exists())
        pending.unlink()
        for status in (
            f'complete {first}',
            f'repair {first}',
            f'working {first} prepare',
            f'failed {first} register 5',
        ):
            receipt.write_text(status + '\n')
            self.assertFalse(self.stage())
            self.assertFalse(pending.exists())
            self.assertTrue(guard.exists())
        value['managed_generation'] = second
        manifest.write_text(json.dumps(value))
        self.assertFalse(self.stage())
        self.assertTrue(guard.is_file())
        self.assertFalse(sentinel.exists())
        self.assertEqual(pending.read_text(), second + '\n')
        self.assertEqual(receipt.read_text(), f'failed {first} register 5\n')

    def test_new_generation_preserves_an_explicit_native_migration_request(self):
        self.stage()
        sentinel = self.data / 'datamigrator/migrate'
        sentinel.parent.mkdir()
        sentinel.touch()
        self.assertTrue(bootstrap_migration.schedule(self.data, PINS['build'], 'a' * 64, self.log.append, None))
        self.assertFalse((self.data / 'Switch/system-app-migration').exists())
        self.assertTrue(sentinel.is_file())
        self.assertEqual((self.installed() / bootstrap_migration.PENDING).read_text(), 'a' * 64 + '\n')

    def test_redeploy_rearms_only_an_interrupted_native_migration_with_both_requests(self):
        self.stage()
        generation = 'a' * 64
        base = self.installed()
        pending = base / bootstrap_migration.PENDING
        receipt = base / bootstrap_migration.RECEIPT
        guard = self.data / 'Switch/system-app-migration'
        sentinel = self.data / 'datamigrator/migrate'
        sentinel.parent.mkdir()
        guard.touch()
        working = f'working {generation} native-migration\n'
        receipt.write_text(working)

        def schedule() -> bool:
            return bootstrap_migration.schedule(self.data, PINS['build'], generation, self.log.append, None)

        self.assertFalse(schedule())
        pending.write_text('b' * 64 + '\n')
        self.assertFalse(schedule())
        pending.write_text(generation + '\n')
        self.assertFalse(schedule())
        sentinel.symlink_to(guard)
        with self.assertRaises(OSError):
            schedule()
        sentinel.unlink()
        sentinel.touch()
        for value in (f'working {generation} preparation\n', f'failed {generation} native-migration 5\n'):
            receipt.write_text(value)
            self.assertFalse(schedule())
        self.assertTrue(guard.exists())

        receipt.write_text(working)
        self.assertTrue(schedule())
        self.assertFalse(guard.exists())
        self.assertFalse(schedule())
        self.assertEqual(receipt.read_text(), working)
        self.assertEqual(pending.read_text(), generation + '\n')
        self.assertEqual(sentinel.read_bytes(), b'')

    def test_corrupt_or_linked_migration_receipts_keep_the_previous_boot_request(self):
        self.stage()
        base = self.installed()
        guard = self.data / 'Switch/system-app-migration'
        guard.touch()
        outside = self.base / 'outside'
        outside.write_bytes(b'unchanged')
        receipt = base / bootstrap_migration.RECEIPT
        pending = base / bootstrap_migration.PENDING

        def schedule() -> bool:
            return bootstrap_migration.schedule(self.data, PINS['build'], 'a' * 64, self.log.append, None)

        for contents in (b'', b'x' * 160, b'complete ' + b'z' * 64 + b'\n', b'failed ' + b'b' * 64 + b' bad 0\n'):
            receipt.write_bytes(contents)
            with self.assertRaises(host.HostError):
                schedule()
            receipt.unlink()
        for name in (receipt, pending, pending.with_suffix('.pending.partial')):
            name.symlink_to(outside)
            with self.assertRaises(OSError):
                schedule()
            name.unlink()
            os.link(outside, name)
            with self.assertRaises(host.HostError):
                schedule()
            name.unlink()
        os.mkfifo(receipt)
        with self.assertRaises(host.HostError):
            schedule()
        receipt.unlink()
        guard.unlink()
        guard.symlink_to(outside)
        receipt.write_text(f'working {"a" * 64} native-migration\n')
        pending.write_text('a' * 64 + '\n')
        with self.assertRaises(OSError):
            schedule()
        self.assertTrue(guard.is_symlink())
        self.assertEqual(pending.read_text(), 'a' * 64 + '\n')
        self.assertEqual(outside.read_bytes(), b'unchanged')

    def test_symlinks_hard_links_modes_and_numeric_ownership_survive(self):
        package(
            self.package,
            [
                regular('var/cache/apt/.keep', b'', uid=501, gid=501),
                link('usr/bin/tool-alias', 'tool'),
                link('usr/bin/hard', 'usr/bin/tool', hard=True),
            ],
        )
        journal = owners.Journal(self.data)
        self.assertTrue(self.stage(journal=journal))
        root = self.installed() / 'root'
        self.assertEqual(os.readlink(root / 'usr/bin/tool-alias'), 'tool')
        self.assertEqual((root / 'usr/bin/tool').stat().st_ino, (root / 'usr/bin/hard').stat().st_ino)
        self.assertEqual((root / 'usr/bin/tool').stat().st_mode & 0o7777, 0o755)
        prefix = 'Switch/bootstrap/21H461/'
        self.assertEqual(journal.wanted[prefix + 'root/var/cache/apt/.keep'], owners.Owner(501, 501, 0o644))
        self.assertNotIn(prefix + bootstrap.HELPER, journal.wanted)
        self.assertFalse((self.installed() / bootstrap.HELPER).exists())

    def test_redeploy_preserves_packages_database_sources_and_initial_state(self):
        self.stage()
        installed = self.installed()
        before = (installed / bootstrap.MANIFEST).read_bytes()
        root = installed / 'root'
        (root / 'usr/bin/tweak').write_bytes(b'user package')
        status = root / 'Library/dpkg/status'
        status.write_text(status.read_text() + '\nPackage: user-tweak\nStatus: install ok installed\nVersion: 1\n')
        (installed / bootstrap.HELPER).write_bytes(b'legacy setuid helper')
        (installed / bootstrap.HELPER).chmod(0o4755)
        package(self.package, [regular('replacement', b'new core')], helper=b'new helper')
        self.assertFalse(self.stage())
        self.assertEqual((root / 'usr/bin/tweak').read_bytes(), b'user package')
        self.assertIn('Package: user-tweak', status.read_text())
        self.assertFalse((root / 'replacement').exists())
        self.assertEqual((installed / bootstrap.MANIFEST).read_bytes(), before)
        self.assertFalse((installed / bootstrap.HELPER).exists())
        self.assertIn('explicit package update required', self.log[-1])

    def test_redeploy_removes_only_the_legacy_helper_symlink(self):
        self.stage()
        outside = self.base / 'user-file'
        outside.write_bytes(b'keep')
        legacy = self.installed() / bootstrap.HELPER
        legacy.symlink_to(outside)
        self.assertFalse(self.stage())
        self.assertFalse(legacy.is_symlink())
        self.assertEqual(outside.read_bytes(), b'keep')

    def test_redeploy_rejects_a_directory_at_the_legacy_helper_path(self):
        self.stage()
        legacy = self.installed() / bootstrap.HELPER
        legacy.mkdir()
        (legacy / 'user-file').write_bytes(b'keep')
        with self.assertRaisesRegex(host.HostError, 'legacy helper'):
            self.stage()
        self.assertEqual((legacy / 'user-file').read_bytes(), b'keep')

    def test_helper_transport_tampering_does_not_publish_bootstrap_state(self):
        helper = bootstrap.payload(self.package).helper
        helper.write_bytes(b'tampered')
        with self.assertRaisesRegex(host.HostError, 'SHA-256'):
            self.stage()
        self.assertFalse(self.installed().exists())
        self.assertFalse((self.data / 'jb').is_symlink())

    def test_redeploy_preserves_legacy_held_uikittools_for_native_package_migration(self):
        self.stage()
        installed = self.installed()
        manifest = installed / bootstrap.MANIFEST
        state = json.loads(manifest.read_text())
        state['holds'] = ['org.coolstar.sileo', 'ellekit', 'uikittools', 'launchctl']
        manifest.write_text(json.dumps(state))
        status = installed / 'root/Library/dpkg/status'
        old = status.read_text().replace(
            'Package: uikittools\nStatus: install ok installed\nVersion: 2.1',
            'Package: uikittools\nStatus: hold ok installed\nVersion: 2.1+switch1',
        )
        status.write_text(old)
        package(self.package, [regular('stock-update', b'new core')])
        self.assertFalse(self.stage())
        self.assertEqual(status.read_text(), old)
        self.assertEqual(
            json.loads(manifest.read_text())['holds'], ['org.coolstar.sileo', 'ellekit', 'uikittools', 'launchctl']
        )

    def test_interrupted_install_never_publishes_a_partial_root(self):
        with (
            mock.patch.object(bootstrap, '_firmware', side_effect=host.HostError('interrupted')),
            self.assertRaisesRegex(host.HostError, 'interrupted'),
        ):
            self.stage()
        self.assertFalse(self.installed().exists())
        self.assertFalse(self.installed().with_suffix('.partial').exists())
        self.assertFalse((self.data / 'jb').is_symlink())
        self.assertTrue(self.stage())

    def test_an_abandoned_staging_directory_is_replaced(self):
        partial = self.installed().with_suffix('.partial')
        partial.mkdir(parents=True)
        (partial / 'unfinished').write_bytes(b'x')
        self.assertTrue(self.stage())
        self.assertFalse((self.installed() / 'unfinished').exists())

    def test_an_interrupted_managed_symlink_publication_is_recovered(self):
        (self.data / 'jb.partial').symlink_to('/private/var/Switch/bootstrap/24B119/root')
        self.assertTrue(self.stage())
        self.assertEqual(os.readlink(self.data / 'jb'), '/private/var/Switch/bootstrap/21H461/root')
        self.assertFalse((self.data / 'jb.partial').is_symlink())

    def test_a_foreign_partial_symlink_is_not_replaced(self):
        (self.data / 'jb.partial').symlink_to('/private/var/other-root')
        with self.assertRaisesRegex(host.HostError, 'unexpected /var/jb.partial'):
            self.stage()
        self.assertEqual(os.readlink(self.data / 'jb.partial'), '/private/var/other-root')

    def test_immediate_ownership_finishes_before_the_tree_is_published(self):
        recorded = []

        class Immediate(owners.Immediate):
            def set(
                self, path: Path, _uid: int, _gid: int, _mode: int | None = None, _flags: int | None = None
            ) -> None:
                recorded.append(Path(path))

        real_rename = Path.rename

        def rename(path: Path, target: Path) -> Path:
            self.assertIn(path / 'root/usr/bin/tool', recorded)
            self.assertIn(path / bootstrap.MANIFEST, recorded)
            return real_rename(path, target)

        with mock.patch.object(Path, 'rename', rename):
            self.assertTrue(self.stage(journal=Immediate()))

    def test_interrupted_immediate_ownership_never_publishes_initialized_state(self):
        class Interrupted(owners.Immediate):
            def set(
                self, _path: Path, _uid: int, _gid: int, _mode: int | None = None, _flags: int | None = None
            ) -> None:
                raise OSError('interrupted chown')

        with self.assertRaisesRegex(OSError, 'interrupted chown'):
            self.stage(journal=Interrupted())
        self.assertFalse(self.installed().exists())
        self.assertFalse((self.data / 'jb').is_symlink())
        self.assertTrue(self.stage())

    def test_hash_and_build_mismatches_are_rejected(self):
        archive = self.package / 'bootstrap' / bootstrap.ARCHIVE
        archive.write_bytes(archive.read_bytes() + b'tampered')
        with self.assertRaisesRegex(host.HostError, 'SHA-256'):
            self.stage()
        package(self.package)
        manifest = self.package / 'bootstrap' / bootstrap.MANIFEST
        value = json.loads(manifest.read_text())
        value['build'] = '24B119'
        manifest.write_text(json.dumps(value))
        with self.assertRaisesRegex(host.HostError, 'differs from selected IPSW'):
            self.stage()

    def test_archive_paths_cannot_escape_or_follow_symlink_ancestors(self):
        for entries in (
            [regular('../outside')],
            [link('escape', str(self.base)), regular('escape/outside')],
            [regular('/outside')],
            [link('hard', '../outside', hard=True)],
            [regular('duplicate'), regular('duplicate')],
        ):
            with self.subTest(entries=[item[0].name for item in entries]):
                package(self.package, entries)
                with self.assertRaises(host.HostError):
                    self.stage()
                self.assertFalse((self.base / 'outside').exists())
                self.assertFalse(self.installed().exists())

    def test_existing_data_and_unmanaged_jailbreak_paths_are_not_followed(self):
        outside = self.base / 'outside'
        outside.mkdir()
        (self.data / 'Switch').symlink_to(outside)
        with self.assertRaisesRegex(host.HostError, 'not a real directory'):
            self.stage()
        self.assertEqual(list(outside.iterdir()), [])

    def test_an_unmanaged_rootless_install_is_refused_before_staging(self):
        (self.data / 'jb').mkdir()
        (self.data / 'jb/user-package').write_bytes(b'keep')
        with self.assertRaisesRegex(host.HostError, '/var/jb already exists'):
            self.stage()
        self.assertEqual((self.data / 'jb/user-package').read_bytes(), b'keep')
        self.assertFalse((self.data / 'Switch').exists())

    def test_install_manifest_describes_the_bootstrap_identity(self):
        spec = {
            'cryptexes': [],
            'volumes': {'data': {'name': 'Data', 'uuid': 'D'}},
            'partition_type': 'apfs',
            'partition_name': 'iOS',
        }
        pins = dict(
            PINS,
            product='iPad7,1',
            device_class='j120ap',
            url='https://example.invalid/os',
            size=1,
            members={'os': {'member': 'os.dmg', 'size': 1, 'content_sha256': 'a' * 64}},
        )
        spec_file, pins_file, out = self.base / 'spec.json', self.base / 'pins.json', self.base / 'out'
        spec_file.write_text(json.dumps(spec))
        pins_file.write_text(json.dumps(pins))
        ini = self.package / 'bootloader/ini/ios.ini'
        ini.parent.mkdir(parents=True)
        ini.write_text('[iOS]\n')
        (self.package / 'switchroot/ios').mkdir(parents=True)
        with mock.patch(
            'sys.argv',
            [
                'artifacts',
                '--spec',
                str(spec_file),
                '--ipsw',
                str(pins_file),
                '--boot-package',
                str(self.package),
                '--out',
                str(out),
            ],
        ):
            artifacts.main()
        recorded = json.loads((out / 'manifest.json').read_text())['bootstrap']
        self.assertEqual(recorded, bootstrap.payload(self.package).manifest)

    def test_a_legacy_boot_package_leaves_bootstrap_data_alone(self):
        self.assertFalse(bootstrap.stage(self.data, self.base / 'legacy', {}, self.log.append))
        self.assertEqual(list(self.data.iterdir()), [])


if __name__ == '__main__':
    unittest.main()
