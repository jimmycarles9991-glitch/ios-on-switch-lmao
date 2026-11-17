"""The SD install's description takes each cryptex at the name the kernel mounts it by, encrypted member or not."""

import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools.install import artifacts

STAGING = '/private/preboot/Switch-Staging/'
IOS17 = {'system_cryptex': {'member': '044-76034-038.dmg', 'deflated': True}}
IOS27 = {'system_cryptex': {'member': '141-10410-227.dmg.aea', 'deflated': False, 'aea_key': 'base64:AAAA'}}


class ArtifactsTest(unittest.TestCase):
    def setUp(self) -> None:
        self.dir = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.dir)
        package = self.dir / 'package'
        (package / 'bootloader/ini').mkdir(parents=True)
        (package / 'bootloader/ini/ios.ini').write_text('[iOS]\n')
        (package / 'switchroot/ios').mkdir(parents=True)
        (package / 'switchroot/ios/kernel').write_bytes(b'k')

    def describe(self, members: dict, kernel_name: str) -> list[dict]:
        spec = {
            'partition_type': 'T',
            'partition_name': 'iOS',
            'volumes': {'data': {'name': 'Data', 'uuid': 'D'}},
            'cryptexes': [{'member': 'system_cryptex', 'path': STAGING + kernel_name, 'volume_uuid': 'S'}],
        }
        pins = {k: k for k in ('product', 'device_class', 'version', 'build', 'url', 'size')}
        pins['members'] = {'os': {'member': 'os.dmg', 'size': 1, 'content_sha256': 'o'}}
        for role, pin in members.items():
            pins['members'][role] = {'size': 2, 'content_sha256': 'c', **pin}
        (self.dir / 'spec.json').write_text(json.dumps(spec))
        (self.dir / 'ipsw.json').write_text(json.dumps(pins))
        out = self.dir / 'out'
        argv = ['artifacts', '--spec', self.dir / 'spec.json', '--ipsw', self.dir / 'ipsw.json']
        argv += ['--boot-package', self.dir / 'package', '--out', out]
        with mock.patch.object(sys, 'argv', [str(a) for a in argv]):
            artifacts.main()
        return json.loads((out / 'preboot-staging.json').read_text())

    def test_a_stored_member_is_staged_under_its_own_name(self) -> None:
        staging = self.describe(IOS17, '044-76034-038.dmg')
        self.assertEqual(staging[0]['ipsw_member'], '044-76034-038.dmg')

    def test_an_encrypted_member_is_staged_under_its_decrypted_name(self) -> None:
        staging = self.describe(IOS27, '141-10410-227.dmg')
        self.assertEqual(staging[0]['preboot_path'], STAGING + '141-10410-227.dmg')
        self.assertEqual(staging[0]['ipsw_member'], '141-10410-227.dmg.aea')

    def test_a_kernel_path_naming_the_archive_is_refused(self) -> None:
        with self.assertRaises(SystemExit):
            self.describe(IOS27, '141-10410-227.dmg.aea')

    def test_another_member_name_is_refused(self) -> None:
        with self.assertRaises(SystemExit):
            self.describe(IOS17, '044-00000-000.dmg')


if __name__ == '__main__':
    unittest.main()
