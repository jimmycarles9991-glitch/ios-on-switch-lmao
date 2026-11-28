import copy
import hashlib
import json
import plistlib
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import configuration


class ConfigurationTest(unittest.TestCase):
    def manifest(self) -> dict:
        return json.loads((Path(__file__).parent / 'test/core_manifest.json').read_text())

    def test_managed_generation_recipe(self) -> None:
        manifest = self.manifest()
        self.assertEqual(
            manifest['managed_generation'], 'a81182d81f619b53a798f4effe0b5a0caa3ab315df3ff916d3a5bbada6467b28'
        )
        self.assertEqual(configuration.managed_packages(manifest), manifest['managed_packages'])
        reversed_manifest = copy.deepcopy(manifest)
        reversed_manifest['managed_packages'] = dict(reversed(list(manifest['managed_packages'].items())))
        self.assertEqual(configuration.managed_packages(reversed_manifest), manifest['managed_packages'])

    def test_managed_packages_are_individual_untrusted_overlay_data(self) -> None:
        root = Path(__file__).absolute().parents[2]
        manifest = json.loads((root / 'overlay/manifest.json').read_text())
        for filename in configuration.MANAGED.values():
            path = '/usr/share/Switch/bootstrap/packages/' + filename
            entry = next(entry for entry in manifest['files'] if entry['path'] == path)
            self.assertEqual(entry['source'], 'build/switch-bootstrap/packages/' + filename)
            self.assertEqual(entry['union'], '/usr/share')
            self.assertNotIn('kind', entry)
            self.assertNotIn('trust', entry)

    def test_native_root_job_remains_available_for_recovery(self) -> None:
        root = Path(__file__).absolute().parents[2]
        manifest = json.loads((root / 'overlay/manifest.json').read_text())
        executable = '/usr/libexec/switch-bootstrap'
        label = 'com.brunostjohn.switch.bootstrap'
        helper = next(entry for entry in manifest['files'] if entry['path'] == executable)
        self.assertTrue(helper['trust'])
        self.assertEqual(helper.get('mode', '0755'), '0755')
        job = next(entry for entry in manifest['files'] if entry['path'].endswith('/' + label + '.plist'))
        self.assertEqual(job['kind'], '24B119')
        self.assertEqual(job['union'], '/private/preboot/Cryptexes/Supplemental')
        self.assertNotIn('boot-arg', job)
        self.assertNotIn('debug', job)
        contents = plistlib.loads((root / job['source']).read_bytes())
        self.assertEqual(contents['Label'], label)
        self.assertEqual(contents['ProgramArguments'], [executable])
        self.assertEqual(contents['UserName'], 'root')
        self.assertTrue(contents['RunAtLoad'])
        self.assertNotIn('KeepAlive', contents)
        self.assertNotIn('_PanicOnCrash', contents)
        limits = plistlib.loads(
            (root / 'overlay/supplemental/com.brunostjohn.switch.bootstrap.jetsam.plist').read_bytes()
        )['Version4']['Daemon']['Override'][label]
        self.assertEqual(limits['ActiveSoftMemoryLimit'], limits['InactiveHardMemoryLimit'])
        self.assertGreater(limits['ActiveSoftMemoryLimit'], 6)

    def test_pins_payload_in_signed_helper_and_helper_in_transport_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, helper, output = (root / name for name in ('manifest.json', 'helper', 'output'))
            digest = 'a' * 64
            initial = self.manifest()
            manifest.write_text(json.dumps(initial))
            helper.write_bytes(b'the signed helper')
            arguments = ['configuration', '--manifest', str(manifest), '--out', str(output)]
            with mock.patch.object(sys, 'argv', arguments):
                configuration.main()
            header = output.read_text()
            self.assertIn(f'#define SWITCH_BOOTSTRAP_ARCHIVE_SHA256 "{digest}"\n', header)
            self.assertIn(f'#define SWITCH_BOOTSTRAP_GENERATION "{initial["managed_generation"]}"\n', header)
            self.assertIn(f'#define SWITCH_BOOTSTRAP_SILEO_SHA256 "{initial["sileo_executable_sha256"]}"\n', header)
            self.assertIn('#define SWITCH_BOOTSTRAP_MANAGED_PACKAGES @{', header)
            for name, package in initial['managed_packages'].items():
                self.assertIn('@' + json.dumps(name), header)
                self.assertIn('@' + json.dumps(package['sha256']), header)
            arguments += ['--helper', str(helper), '--build', '21H461', '--firmware', '17.7.11']
            with mock.patch.object(sys, 'argv', arguments):
                configuration.main()
            result = json.loads(output.read_text())
            self.assertEqual(result['helper_sha256'], hashlib.sha256(helper.read_bytes()).hexdigest())
            self.assertEqual((result['build'], result['firmware']), ('21H461', '17.7.11'))
            self.assertEqual(result['managed_packages'], initial['managed_packages'])
            self.assertEqual(result['managed_generation'], initial['managed_generation'])

    def test_rejects_invalid_managed_packages_without_publishing_output(self) -> None:
        for field, invalid in (
            ('file', '../sileo.deb'),
            ('sha256', 'A' * 64),
            ('size', True),
            ('size', 0),
            ('size', 256 * 1024 * 1024 + 1),
            ('version', '1\nmalformed'),
        ):
            with self.subTest(field=field, invalid=invalid):
                manifest = self.manifest()
                manifest['managed_packages']['org.coolstar.sileo'][field] = invalid
                self.reject_manifest(manifest)
        manifest = self.manifest()
        del manifest['managed_packages']['ellekit']
        self.reject_manifest(manifest)
        manifest = self.manifest()
        manifest['managed_packages']['unexpected'] = manifest['managed_packages']['ellekit']
        self.reject_manifest(manifest)
        manifest = self.manifest()
        manifest['managed_generation'] = 'b' * 64
        self.reject_manifest(manifest)
        manifest = self.manifest()
        manifest['sileo_executable_sha256'] = 'A' * 64
        self.reject_manifest(manifest)
        del manifest['sileo_executable_sha256']
        self.reject_manifest(manifest)

    def test_a_development_image_manages_its_ssh_set_after_the_four(self) -> None:
        manifest = self.manifest()
        records = manifest['managed_packages']
        for name, filename in configuration.DEVELOPMENT.items():
            records[name] = dict(records['ellekit'], file=filename)
        manifest['managed_generation'] = hashlib.sha256(
            b'Switch bootstrap migration v1\n' + json.dumps(records, sort_keys=True, separators=(',', ':')).encode()
        ).hexdigest()
        self.assertEqual(configuration.managed_packages(manifest), records)
        header = configuration.package_header(records)
        order = [header.index('@' + json.dumps(name) + ': @{') for name in configuration.MANAGED]
        order += [header.index('@' + json.dumps(name) + ': @{') for name in configuration.DEVELOPMENT]
        self.assertEqual(order, sorted(order))
        for name in configuration.DEVELOPMENT:
            partial = copy.deepcopy(manifest)
            del partial['managed_packages'][name]
            self.reject_manifest(partial)

    def reject_manifest(self, manifest: dict) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source, output = Path(temporary) / 'manifest', Path(temporary) / 'output'
            source.write_text(json.dumps(manifest))
            with (
                mock.patch.object(sys, 'argv', ['configuration', '--manifest', str(source), '--out', str(output)]),
                self.assertRaises(ValueError),
            ):
                configuration.main()
            self.assertFalse(output.exists())

    def test_rejects_invalid_manifest_without_publishing_output(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            manifest, output = Path(temporary) / 'manifest', Path(temporary) / 'output'
            manifest.write_text('{"schema_version":1,"archive_sha256":"untrusted"}')
            with (
                mock.patch.object(sys, 'argv', ['configuration', '--manifest', str(manifest), '--out', str(output)]),
                self.assertRaises(ValueError),
            ):
                configuration.main()
            self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main()
