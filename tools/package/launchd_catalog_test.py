import hashlib
import plistlib
import tempfile
import unittest
from pathlib import Path

from launchd_catalog import (
    APPS_JOB_PATH,
    APPS_LABEL,
    JOB_PATH,
    LABEL,
    SDWAKE_JOB_PATH,
    add_bootstrap_jobs,
    read_pinned_catalog,
)


class LaunchdCatalogTest(unittest.TestCase):
    def setUp(self):
        self.native = {
            'AppExtensions': {'some-extension': {'Path': '/System/Library/test'}},
            'LaunchDaemons': {'/System/Library/LaunchDaemons/apple.plist': {'Label': 'com.apple.fixture'}},
            'SystemLibraryTreeState': {'marker': b'bytes'},
        }
        self.catalog = plistlib.dumps(self.native, fmt=plistlib.FMT_BINARY)
        self.job = {
            'Label': LABEL,
            'ProgramArguments': ['/usr/libexec/switch-bootstrap'],
            'UserName': 'root',
            'RunAtLoad': True,
        }
        service = Path('drivers/SwitchAppRegistration/com.brunostjohn.switch.bootstrap-apps.plist')
        self.apps_job = plistlib.loads(service.read_bytes())
        sdwake = Path('drivers/NativeSwitchBlock/helper/com.brunostjohn.switch.sdwake.plist')
        self.sdwake_job = plistlib.loads(sdwake.read_bytes())

    def test_existing_catalogue_entries_are_kept(self):
        result = add_bootstrap_jobs(self.catalog, self.job, self.apps_job, self.sdwake_job)
        self.assertEqual(result, add_bootstrap_jobs(self.catalog, self.job, self.apps_job, self.sdwake_job))
        value = plistlib.loads(result, fmt=plistlib.FMT_BINARY)
        self.assertEqual(value['LaunchDaemons'].pop(JOB_PATH), self.job)
        self.assertEqual(value['LaunchDaemons'].pop(APPS_JOB_PATH), self.apps_job)
        self.assertEqual(value['LaunchDaemons'].pop(SDWAKE_JOB_PATH), self.sdwake_job)
        self.assertEqual(value, self.native)

    def test_existing_label_cannot_be_replaced_through_another_path(self):
        for existing in (self.job, self.apps_job, self.sdwake_job):
            self.native['LaunchDaemons']['/existing.plist'] = existing
            with (
                self.subTest(label=existing['Label']),
                self.assertRaisesRegex(ValueError, 'already in the native catalogue'),
            ):
                add_bootstrap_jobs(
                    plistlib.dumps(self.native, fmt=plistlib.FMT_BINARY), self.job, self.apps_job, self.sdwake_job
                )

    def test_app_registration_job_is_mobile_and_on_demand(self):
        for change in (
            {'UserName': 'root'},
            {'MachServices': {APPS_LABEL: 1}},
            {'MachServices': {APPS_LABEL: True, 'other.service': True}},
            {'ProgramArguments': ['/usr/libexec/switch-appregd']},
            {'RunAtLoad': True},
            {'KeepAlive': True},
        ):
            with self.subTest(change=change), self.assertRaisesRegex(ValueError, 'invalid bootstrap app registration'):
                add_bootstrap_jobs(self.catalog, self.job, self.apps_job | change, self.sdwake_job)

    def test_sdwake_job_is_root_and_iokit_event_driven(self):
        for change in (
            {'UserName': 'mobile'},
            {'ProgramArguments': ['/usr/libexec/other']},
            {'LaunchEvents': {}},
            {'RunAtLoad': True},
            {'KeepAlive': True},
        ):
            with self.subTest(change=change), self.assertRaisesRegex(ValueError, 'invalid shared SD wake'):
                add_bootstrap_jobs(self.catalog, self.job, self.apps_job, self.sdwake_job | change)

    def test_both_input_files_must_match_their_pins(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            catalogue = root / 'launchd.plist'
            catalogue.write_bytes(self.catalog)
            signature = root / 'launchd.plist.sig'
            signature.write_bytes(b'native signature')
            catalog_hash = hashlib.sha256(self.catalog).hexdigest()
            signature_hash = hashlib.sha256(b'native signature').hexdigest()
            self.assertEqual(read_pinned_catalog(catalogue, signature, catalog_hash, signature_hash), self.catalog)
            with self.assertRaisesRegex(ValueError, 'differs from the IPSW pins'):
                read_pinned_catalog(catalogue, signature, 'bad', signature_hash)
            signature.write_bytes(b'altered signature')
            with self.assertRaisesRegex(ValueError, 'differs from the IPSW pins'):
                read_pinned_catalog(catalogue, signature, catalog_hash, signature_hash)


if __name__ == '__main__':
    unittest.main()
