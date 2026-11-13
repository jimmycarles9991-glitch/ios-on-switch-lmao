import hashlib
import plistlib
import tempfile
import unittest
import zipfile
from pathlib import Path

from tools.gpu.shaders import inputs
from tools.gpu.shaders.metallib import KERNEL
from tools.gpu.shaders.metallib_test import _library


class ShaderInputsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.crawl = self.root / 'crawl'
        (self.crawl / 'root/System').mkdir(parents=True)
        (self.crawl / 'data').mkdir()
        self.system = _library([('system_kernel', KERNEL, [])])
        (self.crawl / 'root/System/default.metallib').write_bytes(self.system)
        self.library = _library([('app_kernel', KERNEL, [])])
        self.out = self.root / 'out'

    def archive(
        self, minimum: str = '27.0', platform: str = 'iPhoneOS', libraries: bool = True, extra: str | None = None
    ):
        archive = self.root / 'test.ipa'
        info = {
            'MinimumOSVersion': minimum,
            'CFBundleSupportedPlatforms': [platform],
            'CFBundleIdentifier': 'com.brunostjohn.switch.fixture',
        }
        with zipfile.ZipFile(archive, 'w') as bundle:
            bundle.writestr('Payload/Fixture.app/Info.plist', plistlib.dumps(info))
            if libraries:
                bundle.writestr('Payload/Fixture.app/default.metallib', self.library)
                bundle.writestr('Payload/Fixture.app/Effects/nested.metallib', self.library)
            if extra:
                bundle.writestr(extra, self.library)
        return archive

    def test_exact_application_paths_and_os_identity(self):
        report = inputs.prepare(self.crawl, [self.archive()], [], '27', self.out)
        self.assertEqual((self.out / 'root/System/default.metallib').read_bytes(), self.system)
        self.assertEqual((self.crawl / 'root/System/default.metallib').read_bytes(), self.system)
        app = report['applications'][0]
        self.assertEqual(app['minimumOs'], '27.0')
        self.assertEqual(app['bundle'], 'com.brunostjohn.switch.fixture')
        self.assertEqual(len(app['libraries']), 2)
        for entry in app['libraries']:
            self.assertEqual((self.out / entry['path']).read_bytes(), self.library)
            self.assertEqual(entry['sha256'], hashlib.sha256(self.library).hexdigest())
        self.assertFalse((self.crawl / 'root/Applications').exists())

    def test_other_os_and_simulator_cannot_enter_seed(self):
        for minimum, platform in [('17.4', 'iPhoneOS'), ('27.0', 'iPhoneSimulator')]:
            with self.subTest(minimum=minimum, platform=platform), self.assertRaises(ValueError):
                inputs.application(self.archive(minimum, platform), self.out, '27')

    def test_embedded_library_is_available_by_content_hash(self):
        header = self.root / 'embedded.h'
        header.write_text('const unsigned char shader[] = {' + ','.join(f'0x{x:02x}' for x in self.library) + '};')
        report = inputs.prepare(self.crawl, [], [header], '17', self.out)
        entry = report['embedded'][0]
        self.assertEqual(entry['path'], 'data/' + hashlib.sha256(self.library).hexdigest() + '.metallib')
        self.assertEqual((self.out / entry['path']).read_bytes(), self.library)
        self.assertEqual(entry['functions'], 1)

    def test_no_library_and_ambiguous_bundle_are_errors(self):
        with self.assertRaises(ValueError):
            inputs.application(self.archive(libraries=False), self.out, '27')
        with self.assertRaises(ValueError):
            inputs.application(self.archive(extra='Payload/Other.app/Info.plist'), self.out, '27')

    def test_conflicting_installed_library_is_not_overwritten(self):
        path = self.crawl / 'root/Applications/Fixture.app/default.metallib'
        path.parent.mkdir(parents=True)
        path.write_bytes(self.system)
        with self.assertRaises(ValueError):
            inputs.prepare(self.crawl, [self.archive()], [], '27', self.out)
        self.assertEqual(path.read_bytes(), self.system)
        self.assertEqual((self.out / path.relative_to(self.crawl)).read_bytes(), self.system)

    def test_invalid_library_and_archive_paths_are_rejected(self):
        with self.assertRaises(ValueError):
            inputs.put_library(self.out, 'data/bad.metallib', b'not Metal')
        with self.assertRaises(ValueError):
            inputs.application(self.archive(extra='Payload/Fixture.app/../../outside.metallib'), self.out, '27')
        self.assertFalse((self.root / 'outside.metallib').exists())


if __name__ == '__main__':
    unittest.main()
