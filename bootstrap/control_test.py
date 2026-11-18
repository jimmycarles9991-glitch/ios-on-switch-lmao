import hashlib
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from bootstrap import packages
from bootstrap.assemble import (
    adapt_prep,
    adapt_sileo_control,
    control_package,
    dependency_present,
    managed_package_manifest,
    read_control,
)


class ControlTests(unittest.TestCase):
    def test_direct_managed_packages_match_actual_deb_identity_and_bytes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            names = {
                'org.coolstar.sileo': 'sileo.deb',
                'ellekit': 'ellekit.deb',
                'uikittools': 'uikittools.deb',
                'launchctl': 'launchctl.deb',
            }
            for name, filename in names.items():
                packages.write_deb(root / filename, control_package(name, '1:2.3+switch1'), {})
            manifest = managed_package_manifest(root)
            self.assertEqual(set(manifest), set(names))
            for name, filename in names.items():
                data = (root / filename).read_bytes()
                self.assertEqual(
                    manifest[name],
                    {
                        'file': filename,
                        'sha256': hashlib.sha256(data).hexdigest(),
                        'size': len(data),
                        'version': '1:2.3+switch1',
                    },
                )
            packages.write_deb(root / 'sileo.deb', control_package('unexpected', '1'), {})
            with self.assertRaisesRegex(ValueError, 'invalid managed package identity'):
                managed_package_manifest(root)

    def test_rootless_library_dependencies_require_present_targets(self):
        entries = {}
        packages.add_file(entries, 'usr/lib/libellekit.dylib', b'library')
        packages.add_link(entries, 'usr/lib/libsubstrate.dylib', 'libellekit.dylib')
        self.assertTrue(dependency_present(entries, '@rpath/libsubstrate.dylib', ['/var/jb/usr/lib']))
        self.assertFalse(dependency_present(entries, '@rpath/missing.dylib', ['/var/jb/usr/lib']))
        self.assertFalse(dependency_present(entries, '/usr/lib/libellekit.dylib', []))

    def test_preparation_stops_at_first_failure(self):
        entries = {}
        packages.add_file(entries, 'prep_bootstrap.sh', b'#!/var/jb/bin/sh\nfalse\ntouch "$1"\nrm -f "$0"\n', 0o755)
        adapt_prep(entries)
        with tempfile.TemporaryDirectory() as temporary:
            script, marker = Path(temporary) / 'prepare', Path(temporary) / 'complete'
            script.write_bytes(entries['prep_bootstrap.sh'][1])
            result = subprocess.run(['/bin/sh', str(script), str(marker)], check=False, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertTrue(script.exists())
            self.assertFalse(marker.exists())

    def test_sileo_retains_tweak_respring_trigger(self):
        release = next(Path(os.environ['TEST_SRCDIR']).glob('*bootstrap_sileo_release/file/downloaded'))
        control = read_control(release)
        adapt_sileo_control(control)
        with tempfile.TemporaryDirectory() as temporary:
            script = Path(temporary) / 'postinst'
            script.write_bytes(control['postinst'][1])
            read_fd, write_fd = os.pipe()
            try:
                subprocess.run(
                    ['/bin/sh', str(script), 'triggered', '/var/jb/Library/MobileSubstrate/DynamicLibraries'],
                    env=dict(os.environ, SILEO=f'{write_fd} 1'),
                    pass_fds=(write_fd,),
                    check=True,
                    capture_output=True,
                )
            finally:
                os.close(write_fd)
            try:
                self.assertEqual(os.read(read_fd, 1024), b'finish:restart\n')
            finally:
                os.close(read_fd)


if __name__ == '__main__':
    unittest.main()
