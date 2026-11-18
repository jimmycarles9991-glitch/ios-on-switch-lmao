import json
import plistlib
import shutil
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

from tools.common import seed_manifest
from tools.install import card, host, owners, staged_apps


class StagedAppsTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.data = Path(temporary.name)
        self.source = self.app(staged_apps.SEEDS, '8')
        self.target = self.data / staged_apps.STAGED / self.source.name
        self.partial = self.target.with_name('.' + self.target.name + '.partial')
        placeholder = self.data / staged_apps.PLACEHOLDERS / self.source.name
        placeholder.mkdir(parents=True)
        shutil.copy(self.source / 'Info.plist', placeholder)
        signatures = self.data / staged_apps.SIGNATURES
        signatures.mkdir(parents=True)
        (signatures / 'com.brunostjohn.switch.installer').write_bytes(b'detached signature')
        self.journal = owners.Journal(self.data)
        self.log = mock.Mock()

    def app(self, parent: str, version: str):
        app = self.data / parent / 'SwitchInstaller.app'
        app.mkdir(parents=True, exist_ok=True)
        info = {
            'CFBundleIdentifier': 'com.brunostjohn.switch.installer',
            'CFBundleExecutable': 'SwitchInstaller',
            'CFBundleVersion': version,
        }
        (app / 'Info.plist').write_bytes(plistlib.dumps(info))
        (app / 'SwitchInstaller').write_bytes(b'signed executable')
        (app / 'SwitchInstaller').chmod(0o755)
        (app / '_CodeSignature').mkdir(exist_ok=True)
        (app / '_CodeSignature/CodeResources').write_bytes(b'signed resources')
        return app

    def stage(self):
        seeds = staged_apps._Seeds.read(self.data, self.data / 'Switch/overlay')
        return staged_apps._stage(self.data, seeds, self.journal, self.log)

    def test_first_boot_preserves_signature_modes_links_and_mobile_ownership(self):
        (self.source / 'linked').symlink_to('_CodeSignature')
        self.assertTrue(self.stage())
        self.assertEqual((self.target / 'SwitchInstaller').read_bytes(), b'signed executable')
        self.assertEqual((self.target / 'SwitchInstaller').stat().st_mode & 0o777, 0o755)
        self.assertEqual((self.target / '_CodeSignature/CodeResources').read_bytes(), b'signed resources')
        self.assertEqual((self.target / 'linked').readlink(), Path('_CodeSignature'))
        for path, _ in owners.walk(self.target):
            self.assertEqual(self.journal.wanted[path.relative_to(self.data).as_posix()], owners.Owner(501, 501))

    def test_pending_stage_is_idempotent_and_reapplies_ownership(self):
        self.stage()
        inode = (self.target / 'SwitchInstaller').stat().st_ino
        self.journal = owners.Journal(self.data)
        self.assertTrue(self.stage())
        self.assertEqual((self.target / 'SwitchInstaller').stat().st_ino, inode)
        self.assertTrue(self.journal.wanted)
        self.log.assert_called_once()

    def test_incomplete_or_changed_staging_is_repaired(self):
        self.stage()
        resources = self.target / '_CodeSignature/CodeResources'
        resources.unlink()
        self.partial.mkdir()
        (self.partial / 'unfinished').write_bytes(b'partial')
        self.assertTrue(self.stage())
        self.assertEqual(resources.read_bytes(), b'signed resources')
        self.assertFalse(self.partial.exists())
        (self.target / 'SwitchInstaller').chmod(0o644)
        self.assertTrue(self.stage())
        self.assertEqual((self.target / 'SwitchInstaller').stat().st_mode & 0o777, 0o755)
        resources.write_bytes(b'wrong resources')
        self.assertTrue(self.stage())
        self.assertEqual(resources.read_bytes(), b'signed resources')

    def test_failed_copy_keeps_previous_staging_and_retries(self):
        self.stage()
        (self.source / 'SwitchInstaller').write_bytes(b'new signed executable')
        with (
            mock.patch.object(staged_apps.shutil, 'copytree', side_effect=OSError('full disk')),
            self.assertRaises(OSError),
        ):
            self.stage()
        self.assertEqual((self.target / 'SwitchInstaller').read_bytes(), b'signed executable')
        self.assertFalse(self.partial.exists())
        self.assertTrue(self.stage())
        self.assertEqual((self.target / 'SwitchInstaller').read_bytes(), b'new signed executable')

    def test_unsigned_profile_is_refused_before_replacing_pending_copy(self):
        self.stage()
        profile = self.source / 'embedded.mobileprovision'
        profile.write_bytes(plistlib.dumps({'Entitlements': {}}))
        with self.assertRaisesRegex(host.HostError, 'not a decodable CMS profile'):
            self.stage()
        self.assertFalse((self.target / profile.name).exists())
        self.assertFalse(self.partial.exists())

    def test_image_pin_is_checked_before_replacing_pending_copy(self):
        package = self.data / 'package'
        manifest = package / seed_manifest.MANIFEST
        manifest.parent.mkdir(parents=True)
        ipa = package / 'Installer.ipa'
        with zipfile.ZipFile(ipa, 'w') as archive:
            for path in self.source.rglob('*'):
                if path.is_file():
                    archive.write(path, f'Payload/{self.source.name}/{path.relative_to(self.source)}')
        manifest.write_text(json.dumps(seed_manifest.capture(ipa)))
        self.assertTrue(staged_apps.stage(self.data, self.journal, self.log, package=package))
        (self.source / 'SwitchInstaller').write_bytes(b'other code with the same bundle identifier')
        with self.assertRaisesRegex(host.HostError, 'differs from the image input'):
            staged_apps.stage(self.data, self.journal, self.log, package=package)
        self.assertEqual((self.target / 'SwitchInstaller').read_bytes(), b'signed executable')

    def test_existing_card_refuses_wrong_inherited_owner_before_replacing_staged_app(self):
        self.stage()
        (self.source / 'SwitchInstaller').write_bytes(b'updated executable')
        overlay = self.data / 'Switch/overlay'
        seeds = staged_apps._Seeds.read(self.data, overlay)
        with (
            mock.patch.object(staged_apps, '_mobile_tree', return_value=False),
            self.assertRaisesRegex(host.HostError, 'did not inherit mobile ownership'),
        ):
            staged_apps._stage(self.data, seeds, None, self.log, inherited=True)
        self.assertEqual((self.target / 'SwitchInstaller').read_bytes(), b'signed executable')
        self.assertFalse(self.partial.exists())

    def test_current_or_newer_install_removes_stale_staging(self):
        for version in ('8', '8.0', '9'):
            with self.subTest(version=version):
                self.app(staged_apps.STAGED, '7')
                self.app(staged_apps.INSTALLED + '/container', version)
                self.assertFalse(self.stage())
                self.assertFalse(self.target.exists())

    def test_older_install_stages_upgrade(self):
        self.app(staged_apps.INSTALLED + '/container', '7')
        self.assertTrue(self.stage())
        self.assertEqual(plistlib.loads((self.target / 'Info.plist').read_bytes())['CFBundleVersion'], '8')

    def test_deleted_app_is_not_reintroduced(self):
        self.stage()
        state = self.data / staged_apps.STATES
        state.parent.mkdir(parents=True)
        state.write_bytes(plistlib.dumps({'com.brunostjohn.switch.installer': 2}))
        self.assertFalse(self.stage())
        self.assertFalse(self.target.exists())
        self.assertTrue(self.source.exists())

    def test_malformed_state_does_not_reinstall_a_deleted_app(self):
        state = self.data / staged_apps.STATES
        state.parent.mkdir(parents=True)
        state.write_bytes(b'broken state')
        with self.assertRaises(host.HostError):
            self.stage()
        self.assertFalse(self.target.exists())

    def test_placeholder_install_does_not_count_as_installed(self):
        app = self.app(staged_apps.INSTALLED + '/container', '8')
        (app / 'SwitchInstaller').unlink()
        self.assertTrue(self.stage())

    def test_missing_system_placeholder_refuses_staging(self):
        shutil.rmtree(self.data / staged_apps.PLACEHOLDERS)
        with self.assertRaises(host.HostError):
            self.stage()
        self.assertFalse(self.target.exists())

    def test_missing_detached_signature_refuses_staging(self):
        shutil.rmtree(self.data / staged_apps.SIGNATURES)
        with self.assertRaises(host.HostError):
            self.stage()
        self.assertFalse(self.target.exists())

    def test_no_seed_leaves_apples_staged_apps_alone(self):
        shutil.rmtree(self.source)
        apple = self.data / staged_apps.STAGED / 'Maps.app'
        apple.mkdir(parents=True)
        self.assertFalse(self.stage())
        self.assertTrue(apple.exists())

    def test_upgrade_to_builtin_retires_only_its_seed(self):
        self.stage()
        other = self.target.with_name('Maps.app')
        other.mkdir()
        private = self.data / 'mobile/Containers/Data/Application/container/Documents'
        private.mkdir(parents=True)
        (private / 'saved').write_bytes(b'data')
        package = self.data / 'package'
        built = package / 'overlay/Applications' / self.source.name
        shutil.copytree(self.source, built, copy_function=shutil.copy)
        ipa = package / 'app.ipa'
        with zipfile.ZipFile(ipa, 'w') as archive:
            for path in built.rglob('*'):
                if path.is_file():
                    archive.write(path, f'Payload/{built.name}/{path.relative_to(built)}')
        (package / seed_manifest.BUILTIN_MANIFEST).write_text(json.dumps(seed_manifest.capture(ipa)))
        self.assertTrue(card.sync_overlay(self.data, package, self.log, dry_run=True))
        self.assertTrue(self.target.exists())
        self.assertTrue(card.sync_overlay(self.data, package, self.log))
        self.assertFalse(card.sync_overlay(self.data, package, self.log))
        self.assertFalse(self.target.exists())
        self.assertFalse(self.source.exists())
        self.assertFalse((self.data / staged_apps.PLACEHOLDERS / built.name).exists())
        self.assertFalse((self.data / staged_apps.SIGNATURES / seed_manifest.IDENTIFIER).exists())
        self.assertTrue(other.is_dir())
        self.assertEqual((private / 'saved').read_bytes(), b'data')
        self.assertFalse(staged_apps.stage(self.data, self.journal, self.log, package=package))


if __name__ == '__main__':
    unittest.main()
