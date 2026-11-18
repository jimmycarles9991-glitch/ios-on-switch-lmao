import os
import plistlib
import re
import tempfile
import unittest
import zipfile
from pathlib import Path

from tools.deploy import registration


def bundle(ipa: str) -> tuple[dict, dict, dict]:
    with zipfile.ZipFile(ipa) as archive:
        [info_path] = [name for name in archive.namelist() if re.fullmatch(r'Payload/[^/]+\.app/Info\.plist', name)]
        root = info_path.removesuffix('Info.plist')
        info = plistlib.loads(archive.read(info_path))
        profile_name = root + 'embedded.mobileprovision'
        profile = plistlib.loads(archive.read(profile_name)) if profile_name in archive.namelist() else {}
        with tempfile.TemporaryDirectory() as temporary:
            executable = Path(temporary) / info['CFBundleExecutable']
            executable.write_bytes(archive.read(root + info['CFBundleExecutable']))
            signing = registration.signing(executable)
        entitlements = plistlib.loads(bytes.fromhex(signing['entitlements']['5']))
        return info, entitlements, profile


class SigningTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.apps = {ipa: bundle(ipa) for ipa in os.environ['APPS'].split()}

    def test_both_apps_and_ios_versions_are_checked(self):
        self.assertEqual(
            {(info['CFBundleIdentifier'], info['MinimumOSVersion']) for info, _, _ in self.apps.values()},
            {
                ('com.brunostjohn.switch.hwtest', '17.4'),
                ('com.brunostjohn.switch.hwtest', '27.0'),
                ('com.brunostjohn.switch.installer', '17.4'),
                ('com.brunostjohn.switch.installer', '27.0'),
            },
        )

    def test_fileprovider_can_extract_the_bundle_identifier(self):
        for ipa, (info, entitlements, _) in self.apps.items():
            with self.subTest(ipa=ipa):
                identity = entitlements['application-identifier']
                self.assertRegex(identity, r'^[A-Z0-9]{10}\.')
                self.assertEqual(identity[11:], info['CFBundleIdentifier'])

    def test_profile_identity_and_keychain_agree(self):
        for ipa, (info, entitlements, profile) in self.apps.items():
            with self.subTest(ipa=ipa):
                if info['CFBundleIdentifier'] == 'com.brunostjohn.switch.installer':
                    self.assertFalse(profile, 'the unsigned build fixture is not a device provisioning profile')
                    self.assertEqual(entitlements['keychain-access-groups'], [entitlements['application-identifier']])
                    continue
                [prefix] = profile['ApplicationIdentifierPrefix']
                self.assertRegex(prefix, r'^[A-Z0-9]{10}$')
                self.assertEqual(profile['TeamIdentifier'], [prefix])
                identity = prefix + '.' + info['CFBundleIdentifier']
                self.assertEqual(entitlements['application-identifier'], identity)
                self.assertEqual(entitlements['keychain-access-groups'], [identity])
                self.assertEqual(
                    profile['Entitlements']['application-identifier'], prefix + '.com.brunostjohn.switch.*'
                )

    def test_apps_keep_their_sandbox(self):
        for ipa, (_, entitlements, _) in self.apps.items():
            with self.subTest(ipa=ipa):
                self.assertIs(entitlements['com.apple.private.security.container-required'], True)
                for key in [
                    'com.apple.private.security.no-sandbox',
                    'com.apple.private.security.no-container',
                    'com.apple.private.security.storage.FileProvider',
                ]:
                    self.assertNotIn(key, entitlements)

    def test_installer_can_query_launchservices(self):
        for ipa, (info, entitlements, _) in self.apps.items():
            if info['CFBundleIdentifier'] != 'com.brunostjohn.switch.installer':
                continue
            with self.subTest(ipa=ipa):
                self.assertIs(entitlements.get('com.apple.private.coreservices.canmaplsdatabase'), True)
                modern = info['MinimumOSVersion'] == '27.0'
                self.assertEqual(
                    entitlements['com.apple.private.mobileinstall.allowedSPI'],
                    ['InstallForInstallCoordination' if modern else 'InstallForLaunchServices'],
                )
                self.assertEqual(
                    entitlements['com.apple.security.exception.mach-lookup.global-name'],
                    ['com.apple.mobile.installd' if modern else 'com.apple.lsd.installation'],
                )

    def test_installer_requests_document_copies(self):
        for ipa, (info, _, _) in self.apps.items():
            if info['CFBundleIdentifier'] != 'com.brunostjohn.switch.installer':
                continue
            with self.subTest(ipa=ipa):
                self.assertIs(info.get('LSSupportsOpeningDocumentsInPlace'), True)
                self.assertTrue(
                    any(
                        'com.apple.itunes.ipa' in document.get('LSItemContentTypes', [])
                        and document.get('CFBundleTypeRole') == 'Viewer'
                        for document in info.get('CFBundleDocumentTypes', [])
                    )
                )

    def test_import_diagnostics_follow_the_build_profile(self):
        for ipa, (info, _, _) in self.apps.items():
            if info['CFBundleIdentifier'] != 'com.brunostjohn.switch.installer':
                continue
            with self.subTest(ipa=ipa), zipfile.ZipFile(ipa) as archive:
                [executable] = [
                    name
                    for name in archive.namelist()
                    if re.fullmatch(r'Payload/[^/]+\.app/' + info['CFBundleExecutable'], name)
                ]
                present = b'SwitchImportDiagnostics' in archive.read(executable)
                self.assertEqual(present, os.environ['IMPORT_DIAGNOSTICS'] == '1')
                self.assertEqual(b'MIInstallerClient\0' in archive.read(executable), info['MinimumOSVersion'] == '27.0')


if __name__ == '__main__':
    unittest.main()
