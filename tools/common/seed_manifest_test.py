import plistlib
import tempfile
import unittest
import zipfile
from pathlib import Path

from tools.common import seed_manifest as seed


class InstallerSeed(unittest.TestCase):
    def setUp(self):
        work = tempfile.TemporaryDirectory()
        self.addCleanup(work.cleanup)
        root = Path(work.name)
        self.app = root / seed.APP
        self.files = {
            'Info.plist': plistlib.dumps(
                {
                    'CFBundleIdentifier': seed.IDENTIFIER,
                    'CFBundleExecutable': 'SwitchInstaller',
                    'CFBundleVersion': '10',
                }
            ),
            'SwitchInstaller': b'compiled code and signing data',
            '_CodeSignature/CodeResources': b'resource seal',
            'Assets.car': b'icon',
        }
        self.ipa = root / 'app.ipa'
        with zipfile.ZipFile(self.ipa, 'w') as archive:
            for name, data in self.files.items():
                archive.writestr(f'Payload/{seed.APP}/{name}', data)
                path = self.app / name
                path.parent.mkdir(exist_ok=True, parents=True)
                path.write_bytes(data)
        self.pin = seed.capture(self.ipa)

    def test_same_identifier_is_insufficient(self):
        seed.verify(self.app, self.pin)
        for name in ('SwitchInstaller', '_CodeSignature/CodeResources', 'Assets.car'):
            with self.subTest(name=name):
                path = self.app / name
                path.write_bytes(b'replacement')
                with self.assertRaisesRegex(ValueError, 'differs'):
                    seed.verify(self.app, self.pin)
                path.write_bytes(self.files[name])

    def test_missing_extra_and_linked_resources(self):
        resource = self.app / 'Assets.car'
        resource.unlink()
        with self.assertRaisesRegex(ValueError, 'missing files'):
            seed.verify(self.app, self.pin)
        other = self.app.parent / 'external'
        other.write_bytes(self.files[resource.name])
        resource.symlink_to(other)
        with self.assertRaisesRegex(ValueError, 'differs'):
            seed.verify(self.app, self.pin)
        resource.unlink()
        resource.write_bytes(other.read_bytes())
        (self.app / 'extra').touch()
        with self.assertRaisesRegex(ValueError, 'differs'):
            seed.verify(self.app, self.pin)

    def test_ambiguous_bundle_path_is_not_pinned(self):
        with zipfile.ZipFile(self.ipa, 'a') as archive:
            archive.writestr(f'Payload/{seed.APP}/data/../Info.plist', self.files['Info.plist'])
        with self.assertRaisesRegex(ValueError, 'unsafe'):
            seed.capture(self.ipa)


if __name__ == '__main__':
    unittest.main()
