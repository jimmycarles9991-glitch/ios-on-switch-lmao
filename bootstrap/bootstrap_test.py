import argparse
import io
import json
import os
import tempfile
import unittest
from pathlib import Path

from bootstrap import packages
from bootstrap.assemble import adapt_uikittools, base_provenance, read_control


class BootstrapTests(unittest.TestCase):
    def setUp(self):
        runfiles = Path(os.environ['TEST_SRCDIR'])
        self.root = runfiles / os.environ['TEST_WORKSPACE'] / 'bootstrap'
        self.base = next(runfiles.glob('*bootstrap_procursus_bootstrap/file/downloaded'))

    def test_pinned_download_is_a_complete_rootless_cf1900_bootstrap(self):
        provenance = base_provenance(argparse.Namespace(base=self.base, sources=self.root / 'sources.json'))
        self.assertEqual(provenance['kind'], 'official-prebuilt')
        self.assertEqual(provenance['cfver'], '1900')
        self.assertTrue(provenance['url'].endswith('/1900/bootstrap-iphoneos-arm64.tar.zst'))
        pins = json.loads((self.root / 'sources.json').read_text())
        self.assertNotIn('procursus', {pin['name'] for pin in pins['sources']})
        entries = packages.read_bootstrap(self.base, self.root / 'libzstd.dylib')
        for name in ('Library/dpkg/status', 'bin/sh', 'usr/bin/apt', 'usr/bin/dpkg', '.procursus_strapped'):
            self.assertIn(name, entries)
        self.assertIn(b'Architecture: iphoneos-arm64\n', entries['Library/dpkg/status'][1])

    def test_unpinned_archive_has_no_provenance_override(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary) / 'bootstrap.tar.zst'
            base.write_bytes(b'altered archive')
            args = argparse.Namespace(base=base, sources=self.root / 'sources.json', base_provenance=base)
            with self.assertRaisesRegex(ValueError, 'does not match the pinned release'):
                base_provenance(args)

    def test_native_uicache_package_preserves_other_upstream_tools_and_scripts(self):
        entries = packages.read_bootstrap(self.base, self.root / 'libzstd.dylib')
        before = dict(entries)
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary) / 'uikittools.deb'
            tool = Path(temporary) / 'uicache'
            tool.write_bytes(b'native registrar')
            adapt_uikittools(entries, out, tool, tool)
            control = read_control(out)
            self.assertIn(b'Version: 2.1.7+switch4\n', control['control'][1])
            self.assertNotIn(b'Status:', control['control'][1])
            self.assertIn(b'Status: hold ok installed', entries['Library/dpkg/status'][1])
            for name in ('postinst', 'triggers'):
                original = entries[f'Library/dpkg/info/uikittools.{name}']
                self.assertEqual(control[name][1], original[1])
                self.assertEqual(control[name][0].mode, original[0].mode)
            archive = packages.read_deb(out)['data.tar.gz']
            with packages.tarfile.open(fileobj=io.BytesIO(archive)) as source:
                repaired = packages.read_tar(source)
            self.assertEqual(repaired['usr/bin/uicache'][1], b'native registrar')
            self.assertEqual(entries['usr/bin/uicache'][1], b'native registrar')
            self.assertEqual(repaired['usr/bin/sbreload'][1], b'native registrar')
            self.assertEqual(repaired['usr/bin/uicache'][0].mode, 0o755)
            self.assertEqual(repaired['usr/bin/cfversion'][0].linkname, 'deviceinfo')
            for name, (member, data) in repaired.items():
                if name in ('usr/bin/uicache', 'usr/bin/sbreload'):
                    continue
                original, expected = before[name]
                self.assertEqual(
                    (data, member.uid, member.gid, member.mode, member.linkname),
                    (expected, original.uid, original.gid, original.mode, original.linkname),
                )
        self.assertEqual(
            entries['Library/dpkg/info/uikittools.triggers'][1], before['Library/dpkg/info/uikittools.triggers'][1]
        )

    def test_stock_repair_rejects_missing_pinned_package_files(self):
        entries = packages.read_bootstrap(self.base, self.root / 'libzstd.dylib')
        del entries['usr/bin/uicache']
        with tempfile.TemporaryDirectory() as temporary, self.assertRaisesRegex(ValueError, 'file is missing'):
            adapt_uikittools(
                entries, Path(temporary) / 'broken.deb', Path(temporary) / 'uicache', Path(temporary) / 'sbreload'
            )


if __name__ == '__main__':
    unittest.main()
