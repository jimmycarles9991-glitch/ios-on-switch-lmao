import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools.shell import shell


class ShellTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.state = self.root / 'Application Support' / 'switch-ios'

    def run_main(self, argv: list[str], *, devices: list[str] | None = None) -> list[list[str]]:
        calls: list[list[str]] = []
        udids = ['00008020-ABC123'] if devices is None else devices

        def fake_which(name: str) -> str | None:
            return {'inetcat': '/opt/homebrew/bin/inetcat', 'idevice_id': '/opt/homebrew/bin/idevice_id'}.get(name)

        def fake_run(command: list[str], **kwargs: object) -> mock.Mock:
            if command[0].endswith('idevice_id'):
                return mock.Mock(returncode=0, stdout='\n'.join(udids) + ('\n' if udids else ''), stderr='')
            calls.append(command)
            stdout = kwargs.get('stdout')
            if stdout is not None:
                stdout.write(b'PID COMMAND %CPU\n')
            return mock.Mock(returncode=0)

        with (
            mock.patch.dict(os.environ, {'SWITCH_HOME': str(self.state)}),
            mock.patch.object(shell.shutil, 'which', side_effect=fake_which),
            mock.patch.object(shell.subprocess, 'run', side_effect=fake_run),
        ):
            self.assertEqual(shell.main(argv), 0)
        return calls

    def test_discovers_udid_and_pins_per_udid_with_accept_new_and_no_embedded_keys(self) -> None:
        (command,) = self.run_main(['top', str(self.root / 'top.txt')])
        expected_known = self.state / 'known_hosts'
        self.assertTrue(expected_known.parent.is_dir())
        self.assertIn('ProxyCommand=/opt/homebrew/bin/inetcat -u 00008020-ABC123 22', command)
        self.assertIn('StrictHostKeyChecking=accept-new', command)
        self.assertIn('HostKeyAlias=switch-dev-00008020-ABC123', command)
        self.assertIn(f'UserKnownHostsFile="{expected_known}"', command)
        self.assertIn('BatchMode=yes', command)
        self.assertNotIn('-i', command)
        self.assertNotIn('IdentitiesOnly=yes', command)
        at = command.index('root@switch-dev-00008020-ABC123')
        self.assertEqual(command[at + 1 :], shell.TOP_ARGS)
        self.assertEqual((self.root / 'top.txt').read_bytes(), b'PID COMMAND %CPU\n')

    def test_explicit_identity_and_udid_and_all_actions(self) -> None:
        key = self.root / 'custom_id'
        (command,) = self.run_main(['-i', str(key), '--udid', 'dev42', 'ps', str(self.root / 'ps.txt')])
        self.assertEqual(command[command.index('-i') + 1], str(key))
        self.assertIn('IdentitiesOnly=yes', command)
        self.assertIn('HostKeyAlias=switch-dev-dev42', command)
        self.assertIn('ProxyCommand=/opt/homebrew/bin/inetcat -u dev42 22', command)
        self.assertEqual(command[command.index('root@switch-dev-dev42') + 1 :], shell.PS_ARGS)

        (command,) = self.run_main(['--udid', 'dev42', 'threads', str(self.root / 'threads.txt'), '--', '-s', '500'])
        self.assertEqual(command[command.index('root@switch-dev-dev42') + 1 :], [*shell.THREADS_ARGS, '-s', '500'])

        (command,) = self.run_main(['--udid', 'dev42', 'run', str(self.root / 'u.txt'), '--', 'uname', '-a'])
        self.assertEqual(command[-2:], ['uname', '-a'])

        (command,) = self.run_main(['--udid', 'dev42', 'shell'])
        self.assertNotIn('BatchMode=yes', command)
        self.assertEqual(command[-1], 'root@switch-dev-dev42')

    def test_udid_discovery_rejects_zero_or_multiple_devices(self) -> None:
        for devices, message in (([], 'no iOS device'), (['dev1', 'dev2'], 'multiple iOS devices')):
            with (
                mock.patch.object(shell.shutil, 'which', return_value='/opt/homebrew/bin/idevice_id'),
                mock.patch.object(
                    shell.subprocess,
                    'run',
                    return_value=mock.Mock(returncode=0, stdout='\n'.join(devices), stderr=''),
                ),
                self.assertRaises(SystemExit) as raised,
            ):
                shell.discover_udid(None)
            self.assertIn(message, str(raised.exception))

    def test_requires_out_file_and_inetcat_and_uses_build_working_directory(self) -> None:
        with self.assertRaises(SystemExit):
            shell.main(['top'])
        with (
            mock.patch.object(shell.shutil, 'which', return_value=None),
            self.assertRaises(SystemExit) as raised,
        ):
            shell.main(['--udid', 'abc', 'top', str(self.root / 'x')])
        self.assertIn('inetcat', str(raised.exception))
        with mock.patch.dict(os.environ, {'BUILD_WORKING_DIRECTORY': str(self.root)}):
            self.run_main(['--udid', 'abc', 'threads', 'threads.txt'])
        self.assertTrue((self.root / 'threads.txt').exists())

    def test_build_file_embeds_no_keys_and_no_bootstrap_dependency(self) -> None:
        build_text = (Path(__file__).absolute().parent / 'BUILD.bazel').read_text()
        self.assertNotIn('//bootstrap', build_text)
        self.assertNotIn('client-key', build_text)
        self.assertNotIn('host-public', build_text)


if __name__ == '__main__':
    unittest.main()
