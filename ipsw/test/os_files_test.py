import hashlib
import plistlib
import tempfile
import unittest
from pathlib import Path

from ipsw import os_files


class OsFilesTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name) / 'volume'
        self.out = Path(self.tmp.name) / 'out'
        self._write(os_files.SYSTEM_VERSION, plistlib.dumps({'ProductBuildVersion': '21H461'}))
        self._write(os_files.INSTALLD, b'installd-bytes')
        self.catalog = b'launchd-catalog'
        self.signature = b'launchd-sig'
        self._write(os_files.LAUNCHD_CATALOG, self.catalog)
        self._write(os_files.LAUNCHD_CATALOG_SIG, self.signature)
        self._write(
            'System/Library/DataClassMigrators/LS.migrator/Info.plist',
            plistlib.dumps(
                {'CFBundleIdentifier': 'com.apple.-0LaunchServicesMigrator', 'CFBundleExecutable': 'LSMigrator'}
            ),
        )
        self._write('System/Library/DataClassMigrators/LS.migrator/LSMigrator', b'ls-migrator')
        self._write(os_files.MIGRATION_WRAPPER, b'wrapper')
        self._write(os_files.MIGRATION_RUNNER, b'runner')

    def _write(self, relative: str, data: bytes) -> None:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def test_extracts_pinned_files_and_checks_catalog_digests(self):
        cat_hash = hashlib.sha256(self.catalog).hexdigest()
        sig_hash = hashlib.sha256(self.signature).hexdigest()
        os_files.extract(self.root, '21H461', self.out, cat_hash, sig_hash)
        self.assertEqual((self.out / os_files.INSTALLD).read_bytes(), b'installd-bytes')
        self.assertEqual((self.out / os_files.INSTALLD).stat().st_mode & 0o777, 0o755)
        self.assertEqual((self.out / os_files.LAUNCHD_CATALOG).read_bytes(), self.catalog)
        self.assertEqual((self.out / os_files.LAUNCHD_CATALOG_SIG).read_bytes(), self.signature)
        self.assertEqual(
            (self.out / 'System/Library/DataClassMigrators/LS.migrator/LSMigrator').read_bytes(), b'ls-migrator'
        )
        self.assertEqual((self.out / os_files.MIGRATION_WRAPPER).read_bytes(), b'wrapper')
        self.assertEqual((self.out / os_files.MIGRATION_RUNNER).read_bytes(), b'runner')

    def test_rejects_mismatched_build_or_catalog_pin(self):
        with self.assertRaisesRegex(ValueError, 'differs from pinned'):
            os_files.extract(self.root, '24B119', self.out)
        with self.assertRaisesRegex(ValueError, 'differs from the IPSW pins'):
            os_files.extract(self.root, '21H461', self.out, 'bad', 'bad')


if __name__ == '__main__':
    unittest.main()
