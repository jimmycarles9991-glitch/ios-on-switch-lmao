"""deploy.main's tail: a step that fails once the card is being written leaves the rest and the eject to run."""

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

    def deploy(self, **failing: Exception) -> tuple[int, list[str]]:
        """Run main() against a card of mocks; `failing` names the steps that raise. Return its status and log lines."""
        card, fat = mock.MagicMock(disk='disk9'), mock.MagicMock(identifier='disk9s1')
        spec = {'volumes': {'preboot': {'uuid': 'P'}, 'system': {'uuid': 'S'}, 'data': {'uuid': 'D'}}}
        log = self.root / 'deploy.log'
        argv = ['disk9', '--yes', '--confirm', 'disk9', '--boot-package', 'p', '--spec', 's', '--log', str(log)]
        steps = ('remove_stale_logging_profiles', '_stage_gpu', '_eject')
        with contextlib.ExitStack() as stack:
            for name in steps:
                stack.enter_context(mock.patch.object(deploy, name, self.step(name, failing.get(name))))
            stack.enter_context(
                mock.patch.object(deploy.icon_cache, 'present', self.step('icon_cache', failing.get('icon_cache')))
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

    def test_a_refused_icon_cache_still_ejects_and_fails_the_deploy(self):
        error = denied('containers/Shared/SystemGroup/systemgroup.com.apple.lsd.iconscache/Library')
        status, lines = self.deploy(icon_cache=error)
        self.assertEqual(status, 1)
        self.assertEqual(self.order, ['remove_stale_logging_profiles', '_stage_gpu', 'icon_cache', '_eject'])
        self.assertIn(f'icon cache: NOT done: {error}', lines)
        self.assertEqual(lines[-1], 'FAILED: not done: icon cache')
        self.assertNotIn('deployed', lines)

    def test_a_refused_probe_before_the_gpu_bundle_does_not_keep_it_off_the_card(self):
        status, lines = self.deploy(remove_stale_logging_profiles=denied('preferences/Logging'))
        self.assertEqual(status, 1)
        self.assertEqual(self.order, ['remove_stale_logging_profiles', '_stage_gpu', 'icon_cache', '_eject'])
        self.assertEqual(lines[-1], 'FAILED: not done: stale logging profiles')

    def test_a_deploy_with_every_step_done_says_so(self):
        status, lines = self.deploy()
        self.assertEqual((status, lines[-1]), (0, 'deployed'))
        self.assertEqual(self.order[-1], '_eject')

    def test_a_refused_overlay_stops_the_deploy_with_one_line(self):
        error = denied('Switch/overlay')
        status, lines = self.deploy(overlay=error)
        self.assertEqual(status, 1)
        self.assertEqual(self.order, [])
        self.assertEqual(lines[-1], f'FAILED: {error}')

    def test_a_failed_gpu_bundle_still_stops_the_deploy(self):
        status, lines = self.deploy(_stage_gpu=deploy.host.HostError('GPU bundle NOT staged'))
        self.assertEqual(status, 1)
        self.assertEqual(self.order, ['remove_stale_logging_profiles', '_stage_gpu'])
        self.assertEqual(lines[-1], 'FAILED: GPU bundle NOT staged')


if __name__ == '__main__':
    unittest.main()
