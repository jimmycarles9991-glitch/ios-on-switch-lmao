import plistlib
import subprocess
import tempfile
import unittest
from pathlib import Path

from bootstrap.signing import sign_bundle


class SigningTests(unittest.TestCase):
    def test_bundle_seals_resources_info_and_nested_code(self):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            source = work / 'source.app'
            nested = source / 'PlugIns' / 'nested.appex'
            nested.mkdir(parents=True)
            fixture = work / 'main'
            subprocess.run(
                ['/usr/bin/xcrun', '--sdk', 'macosx', 'clang', '-arch', 'arm64', '-x', 'c', '-', '-o', str(fixture)],
                input=b'int main(void) { return 0; }\n',
                capture_output=True,
                check=True,
            )
            for bundle, identifier in (
                (source, 'com.brunostjohn.switch.fixture'),
                (nested, 'com.brunostjohn.switch.plugin'),
            ):
                (bundle / 'Info.plist').write_bytes(
                    plistlib.dumps(
                        {
                            'CFBundleIdentifier': identifier,
                            'CFBundleExecutable': 'main',
                            'CFBundlePackageType': 'APPL' if bundle == source else 'XPC!',
                        }
                    )
                )
                (bundle / 'main').write_bytes(fixture.read_bytes())
                (bundle / 'main').chmod(0o755)
            (source / 'resource.txt').write_text('sealed resource')
            entitlements = work / 'entitlements.plist'
            entitlements.write_bytes(plistlib.dumps({'com.apple.private.security.no-container': True}))
            signed = sign_bundle(source, work / 'signed.app', entitlements)
            self.assertTrue((signed / '_CodeSignature/CodeResources').is_file())
            self.assertTrue((signed / 'PlugIns/nested.appex/_CodeSignature/CodeResources').is_file())
            for relative in ('Info.plist', 'resource.txt', 'PlugIns/nested.appex/main'):
                with self.subTest(relative=relative):
                    target = signed / relative
                    original = target.read_bytes()
                    target.write_bytes(original + b'changed')
                    result = subprocess.run(
                        ['/usr/bin/codesign', '--verify', '--deep', '--strict', str(signed)],
                        capture_output=True,
                        check=False,
                    )
                    self.assertNotEqual(result.returncode, 0)
                    target.write_bytes(original)


if __name__ == '__main__':
    unittest.main()
