"""The optional external inputs must match the app before deploy opens the card."""

import hashlib
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools.deploy import deploy


class CompilerInputsTest(unittest.TestCase):
    def test_unflagged_deploy_clears_only_compiler_payloads_and_updates_provenance(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            data = root / 'Data'
            bench = data / 'mobile/Media/Switch/benchloop'
            bench.mkdir(parents=True)
            sibling = bench / 'other.bin'
            sibling.write_bytes(b'keep')
            inputs = {key + '.swab': b'payload' for key in deploy.compiler_inputs.STATES}
            for fixtures in (inputs, None, None):
                deploy.compiler_inputs.stage(data, fixtures)
                deploy.deployed.record(data, root, '24B119', lambda *_: None)
                record = json.loads((data / 'Switch/deployed.json').read_text())
                self.assertIs(record['compiler_inputs_staged'], fixtures is not None)
                self.assertEqual(
                    record['compiler_input_states'], list(deploy.compiler_inputs.STATES) if fixtures else []
                )
                self.assertEqual((bench / 'compiler').exists(), fixtures is not None)
                self.assertEqual(sibling.read_bytes(), b'keep')
                self.assertFalse((bench / 'compiler.partial').exists())

    def test_failed_stage_publishes_no_partial_set(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            inputs = {key + '.swab': b'payload' for key in deploy.compiler_inputs.STATES}
            deploy.compiler_inputs.stage(root, inputs)
            with (
                mock.patch.object(deploy.compiler_inputs.files, 'write_bytes', side_effect=OSError('write failed')),
                self.assertRaisesRegex(OSError, 'write failed'),
            ):
                deploy.compiler_inputs.stage(root, inputs)
            self.assertFalse((root / 'mobile/Media/Switch/benchloop/compiler').exists())
            deploy.compiler_inputs.stage(root, None)
            self.assertFalse((root / 'mobile/Media/Switch/benchloop/compiler.partial').exists())

    def test_changed_last_input_stops_deploy_before_opening_the_card(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            executable = root / 'overlay/Applications/SwitchHWTest.app/SwitchHWTest'
            executable.parent.mkdir(parents=True)
            pins = []
            for key in deploy.compiler_inputs.STATES:
                payload = b'SWAB0001' + (16).to_bytes(4, 'little') + key.encode()
                (root / (key + '.swab')).write_bytes(payload)
                pins.append(hashlib.sha256(payload).hexdigest().encode())
            executable.write_bytes(b'\0'.join(pins))
            build = root / 'pins.json'
            build.write_text('{"build":"24B119"}')
            args = deploy._parser().parse_args(
                [
                    'disk9',
                    '--boot-package',
                    str(root),
                    '--compiler-inputs',
                    str(root),
                    '--ipsw',
                    str(build),
                    '--spec',
                    's',
                ]
            )
            expected = deploy.compiler_inputs.load(root, root, '24B119')
            data = root / 'Data'
            deploy.compiler_inputs.stage(data, expected)
            for name, payload in expected.items():
                self.assertEqual((data / 'mobile/Media/Switch/benchloop/compiler' / name).read_bytes(), payload)
            last = root / (deploy.compiler_inputs.STATES[-1] + '.swab')
            last.write_bytes(last.read_bytes() + b'x')
            with (
                mock.patch.object(deploy.host, 'check_tools'),
                mock.patch.object(deploy.pull_logs, 'load_spec', return_value={}),
                mock.patch.object(deploy.cards, 'resolve') as resolve,
                self.assertRaisesRegex(ValueError, last.name),
            ):
                deploy._checked(args, mock.Mock())
            resolve.assert_not_called()

    def test_missing_hwtest_in_package_is_refused_cleanly(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with self.assertRaisesRegex(ValueError, 'does not include SwitchHWTest.app'):
                deploy.compiler_inputs.load(root, root, '24B119')


if __name__ == '__main__':
    unittest.main()
