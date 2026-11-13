"""overlay.py's bundle entries and its Supplemental cryptex rule.

A bundle's .ipa app directory is staged whole, every Mach-O in it trusted; the Supplemental cryptex path holds launchd
plists and launchd's jetsam properties overlay only.
"""

import hashlib
import json
import struct
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

import overlay

APP = '/Applications/SwitchHWTest.app'
SUPPLEMENTAL = '/private/preboot/Cryptexes/Supplemental'


def signed_macho(seed: bytes) -> bytes:
    """Return a thin 64-bit Mach-O whose only load command is LC_CODE_SIGNATURE over one SHA-256 CodeDirectory."""
    directory = bytearray(struct.pack('>II', 0xFADE0C02, 0) + bytes(48) + seed)
    directory[37] = 2
    struct.pack_into('>I', directory, 4, len(directory))
    blob_at = 32 + 16
    superblob = struct.pack('>IIIII', 0xFADE0CC0, 20 + len(directory), 1, 0, 20) + bytes(directory)
    header = struct.pack('<IiiIIIII', 0xFEEDFACF, 0x0100000C, 0, 2, 1, 16, 0, 0)
    command = struct.pack('<IIII', 0x1D, 16, blob_at, len(superblob))
    return header + command + superblob


def directory_hash(image: bytes) -> bytes:
    start = 32 + 16 + 20
    length = struct.unpack_from('>I', image, start + 4)[0]
    return hashlib.sha256(image[start : start + length]).digest()[:20]


class BundleTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.executable = signed_macho(b'main')
        self.dylib = signed_macho(b'dylib')
        self.ipa = self.tmp / 'SwitchHWTest.ipa'
        with zipfile.ZipFile(self.ipa, 'w') as archive:
            archive.writestr('Payload/SwitchHWTest.app/', '')
            archive.writestr('Payload/SwitchHWTest.app/SwitchHWTest', self.executable)
            archive.writestr('Payload/SwitchHWTest.app/Info.plist', b'<plist/>')
            archive.writestr('Payload/SwitchHWTest.app/Frameworks/libswiftCompatibility.dylib', self.dylib)
            archive.writestr('Payload/SwitchHWTest.app/media/h264_720p.mp4', b'\x00\x00\x00\x18ftypmp42')
            archive.writestr('Symbols/ignored', b'x')
        self.manifest = {
            'files': [
                {
                    'path': APP,
                    'source': 'build/switch-hwtest/SwitchHWTest.ipa',
                    'trust': True,
                    'bundle': True,
                    'union': '/Applications',
                }
            ]
        }

    def run_step(self, *argv: str) -> None:
        with mock.patch.object(sys, 'argv', ['overlay.py', *argv]):
            overlay.main()

    def test_plan_unions_the_parent_directory(self):
        given = ['build/switch-hwtest/SwitchHWTest.ipa']
        plan = overlay.plan(self.manifest, portrait=False, kind='21H461', given=given)
        self.assertEqual(plan['dirs'], [['/Applications', '']])
        self.assertTrue(plan['files'][0]['bundle'])

    def test_every_macho_in_the_bundle_is_code(self):
        entry = overlay.plan(self.manifest, portrait=False, kind='21H461')['files'][0]
        self.assertEqual(sorted(overlay.code_of(entry, self.ipa)), sorted([self.executable, self.dylib]))
        self.assertEqual(overlay.cdhash(self.executable), directory_hash(self.executable))

    def test_tree_stages_the_app_directory(self):
        plan = self.tmp / 'plan.json'
        plan.write_text(json.dumps(overlay.plan(self.manifest, portrait=False, kind='21H461')))
        out = self.tmp / 'tree'
        self.run_step(
            'tree', '--plan', str(plan), '--file', f'build/switch-hwtest/SwitchHWTest.ipa={self.ipa}', '--out', str(out)
        )
        app = out / APP.lstrip('/')
        staged = sorted(str(p.relative_to(app)) for p in app.rglob('*') if p.is_file())
        self.assertEqual(
            staged, ['Frameworks/libswiftCompatibility.dylib', 'Info.plist', 'SwitchHWTest', 'media/h264_720p.mp4']
        )
        self.assertEqual((app / 'SwitchHWTest').read_bytes(), self.executable)
        self.assertEqual((app / 'SwitchHWTest').stat().st_mode & 0o777, 0o755)
        self.assertEqual((app / 'Info.plist').stat().st_mode & 0o777, 0o644)

    def test_trust_module_holds_both_cdhashes(self):
        plan = self.tmp / 'plan.json'
        plan.write_text(json.dumps(overlay.plan(self.manifest, portrait=False, kind='21H461')))
        module = self.tmp / 'overlay.trustcache'
        self.run_step(
            'trust',
            '--plan',
            str(plan),
            '--file',
            f'build/switch-hwtest/SwitchHWTest.ipa={self.ipa}',
            '--out',
            str(module),
        )
        data = module.read_bytes()
        for image in (self.executable, self.dylib):
            self.assertIn(directory_hash(image), data)

    def test_an_ipa_without_the_app_is_refused(self):
        planned = overlay.plan(self.manifest, portrait=False, kind='21H461')['files'][0]
        entry = dict(planned, path='/Applications/Other.app')
        with self.assertRaises(SystemExit):
            overlay.code_of(entry, self.ipa)


class SupplementalTest(unittest.TestCase):
    """iOS 27's Supplemental cryptex path takes launchd plists and launchd's jetsam properties overlay, nothing else.

    The commpage byte that makes launchd read it also makes Foundation dlopen Frameworks/ERFoundationExtension.dylib
    from it into every process and Settings show a PreferenceBundles pane from it (and from /private/var/RPSupport).
    """

    @staticmethod
    def manifest(path: str, union: str = SUPPLEMENTAL) -> dict:
        return {'files': [{'path': path, 'source': 'helper.plist', 'union': union, 'kind': '24B119'}]}

    def test_a_launchd_plist_is_allowed(self):
        path = SUPPLEMENTAL + '/System/Library/LaunchDaemons/com.brunostjohn.switch.joyconpaird.plist'
        plan = overlay.plan(self.manifest(path), portrait=False, kind='24B119')
        self.assertEqual(plan['dirs'], [[SUPPLEMENTAL, '']])

    def test_the_jetsam_properties_overlay_is_allowed(self):
        path = SUPPLEMENTAL + '/System/Library/JetsamProperties/com.apple.jetsamproperties.cryptex.plist'
        plan = overlay.plan(self.manifest(path), portrait=False, kind='24B119')
        self.assertEqual([f['path'] for f in plan['files']], [path])

    def test_anything_else_is_refused(self):
        refused = [
            (SUPPLEMENTAL + '/Frameworks/ERFoundationExtension.dylib', SUPPLEMENTAL),
            (SUPPLEMENTAL + '/System/Library/PreferenceBundles/SecurityResearchDeviceERMSettings.bundle', SUPPLEMENTAL),
            (SUPPLEMENTAL + '/System/Library/LaunchDaemons/joyconpaird', SUPPLEMENTAL),
            (SUPPLEMENTAL + '/System/Library/JetsamProperties/com.apple.jetsamproperties.J181.plist', SUPPLEMENTAL),
            (SUPPLEMENTAL + '/System/Library/JetsamProperties/other.plist', SUPPLEMENTAL),
            (SUPPLEMENTAL, '/private/preboot/Cryptexes'),
            ('/System/Cryptexes/Supplemental/Frameworks/ERFoundationExtension.dylib', '/System/Cryptexes/Supplemental'),
            ('/private/var/RPSupport/System/Library/PreferenceBundles/X.bundle', '/private/var/RPSupport'),
        ]
        for path, union in refused:
            with self.subTest(path=path), self.assertRaises(SystemExit):
                overlay.plan(self.manifest(path, union), portrait=False, kind='24B119')

    def test_the_manifest_keeps_the_rule(self):
        manifest = json.loads(Path('overlay/manifest.json').read_text())
        for kind in ('21H461', '24B119'):
            for release in (False, True):
                overlay.plan(manifest, portrait=False, kind=kind, release=release)


if __name__ == '__main__':
    unittest.main()
