"""deploy.main's tail: a step that fails once the card is being written leaves the rest, the boot entry and the eject.

The boot entry comes last: a deploy stopped before it leaves this build's Data under the card's last boot entry, and a
re-run completes it.
"""

import contextlib
import errno
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools.deploy import deploy


def denied(path: str) -> PermissionError:
    return PermissionError(errno.EACCES, 'Permission denied', f'<Data>/{path}')


class DeployTailTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.data = self.root / 'data'
        self.data.mkdir()
        self.order = []

    def tearDown(self):
        self.tmp.cleanup()

    def step(self, name: str, error: Exception | None = None) -> mock.MagicMock:
        def run(*_: object, **__: object) -> None:
            self.order.append(name)
            if error:
                raise error

        return mock.MagicMock(side_effect=run)

    def deploy(self, *extra: str, **failing: Exception) -> tuple[int, list[str]]:
        """Run main() against a card of mocks; `failing` names the steps that raise. Return its status and log lines."""
        card, fat = mock.MagicMock(disk='disk9'), mock.MagicMock(identifier='disk9s1')
        spec = {'volumes': {'preboot': {'uuid': 'P'}, 'system': {'uuid': 'S'}, 'data': {'uuid': 'D'}}}
        log = self.root / 'deploy.log'
        argv = ['disk9', '--yes', '--confirm', 'disk9', '--boot-package', 'p', '--spec', 's', '--log', str(log), *extra]
        steps = ('remove_stale_logging_profiles', '_stage_gpu', '_update_system_cryptex', '_write_boot_entry', '_eject')
        with contextlib.ExitStack() as stack:
            for name in steps:
                stack.enter_context(mock.patch.object(deploy, name, self.step(name, failing.get(name))))
            stack.enter_context(
                mock.patch.object(deploy.data_owner, 'ensure', self.step('Switch/ owner', failing.get('owner')))
            )
            stack.enter_context(
                mock.patch.object(deploy.data_owner, 'authorize', self.step('sudo', failing.get('sudo')))
            )
            stack.enter_context(
                mock.patch.object(deploy, '_checked', return_value=(card, spec, fat, mock.MagicMock(), 'disk10s2'))
            )
            stack.enter_context(
                mock.patch.object(
                    deploy.cards, 'mounted', side_effect=lambda *_, **__: contextlib.nullcontext(self.data)
                )
            )
            stack.enter_context(mock.patch.object(deploy.cards, 'sync_boot_entry', return_value=([], [])))
            stack.enter_context(mock.patch.object(deploy.pull_logs, 'data_volume', return_value=None))
            stack.enter_context(mock.patch.object(deploy.joycon_pairing, 'read', return_value=None))
            stack.enter_context(mock.patch.object(deploy.joycon_pairing, 'stage'))
            stack.enter_context(mock.patch.object(deploy.renamed, 'migrate_data'))
            stack.enter_context(mock.patch.object(deploy.cards, 'sync_overlay', side_effect=failing.get('overlay')))
            stack.enter_context(mock.patch.object(deploy.deployed, 'record'))
            stack.enter_context(mock.patch.object(deploy.system_apps, 'apps', return_value={}))
            stack.enter_context(mock.patch.object(deploy.system_apps, 'register'))
            status = deploy.main(argv)
        return status, [line.split(' ', 1)[1] for line in log.read_text().splitlines()]

    def test_ios_27s_switch_owner_and_system_cryptex_come_before_the_boot_entry_and_a_failure_stops_the_deploy(self):
        ios27 = ('--system-image', 'image', '--pretranslated-trust', 'trust')
        status, _ = self.deploy(*ios27)
        self.assertEqual(status, 0)
        self.assertEqual(self.order[:2], ['sudo', 'Switch/ owner'], "sudo, then Switch/'s owner, before any Data write")
        self.assertEqual(self.order[-3:], ['_update_system_cryptex', '_write_boot_entry', '_eject'])
        self.order.clear()
        status, lines = self.deploy(*ios27, sudo=deploy.host.HostError('sudo: a password is required'))
        self.assertEqual((status, self.order), (1, ['sudo']))
        self.assertEqual(lines[-1], 'FAILED: sudo: a password is required')
        for failing in ('owner', '_update_system_cryptex'):
            with self.subTest(failing=failing):
                self.order.clear()
                status, lines = self.deploy(*ios27, **{failing: deploy.host.HostError('no room')})
                self.assertEqual(status, 1)
                self.assertEqual(lines[-1], 'FAILED: no room')
                self.assertNotIn('_write_boot_entry', self.order)
                self.assertNotIn('_eject', self.order)
                if failing == 'owner':
                    self.assertEqual(self.order, ['sudo', 'Switch/ owner'], 'an owner that does not take stops first')

    def test_ios_17_never_touches_the_system_cryptex(self):
        status, _ = self.deploy()
        self.assertEqual(status, 0)
        self.assertNotIn('_update_system_cryptex', self.order)
        self.assertNotIn('Switch/ owner', self.order)

    def test_a_refused_probe_before_the_gpu_bundle_does_not_keep_it_off_the_card(self):
        status, lines = self.deploy(remove_stale_logging_profiles=denied('preferences/Logging'))
        self.assertEqual(status, 1)
        self.assertEqual(self.order, ['remove_stale_logging_profiles', '_stage_gpu', '_write_boot_entry', '_eject'])
        self.assertEqual(lines[-1], 'FAILED: not done: stale logging profiles')

    def test_a_deploy_with_every_step_done_says_so(self):
        status, lines = self.deploy()
        self.assertEqual((status, lines[-1]), (0, 'deployed'))
        self.assertEqual(self.order[-2:], ['_write_boot_entry', '_eject'])

    def test_a_refused_overlay_stops_the_deploy_with_one_line(self):
        error = denied('Switch/overlay')
        status, lines = self.deploy(overlay=error)
        self.assertEqual(status, 1)
        self.assertEqual(self.order, [])
        self.assertEqual(lines[-1], f'FAILED: {error}')

    def test_an_unreadable_bootstrap_receipt_reports_failure_after_gpu_and_eject(self):
        with mock.patch.object(
            deploy.bootstrap, 'stage', side_effect=denied('Switch/bootstrap/<build>/migration.state')
        ):
            status, lines = self.deploy()
        self.assertEqual(status, 1)
        self.assertIn('_stage_gpu', self.order)
        self.assertEqual(self.order[-1], '_eject')
        self.assertEqual(lines[-1], 'FAILED: not done: bootstrap migration')

    def test_a_failed_gpu_bundle_still_stops_the_deploy(self):
        status, lines = self.deploy(_stage_gpu=deploy.host.HostError('GPU bundle NOT staged'))
        self.assertEqual(status, 1)
        self.assertEqual(self.order, ['remove_stale_logging_profiles', '_stage_gpu'])
        self.assertEqual(lines[-1], 'FAILED: GPU bundle NOT staged')


if __name__ == '__main__':
    unittest.main()
