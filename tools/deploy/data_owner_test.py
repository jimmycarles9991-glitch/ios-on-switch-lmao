"""data_owner: Data's Switch/ read in the check, and set under sudo only when the kernel would refuse it."""

import contextlib
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools.deploy import data_owner
from tools.install import card as cards, host

ROOT_OWNED = data_owner.Owner(0, 0, 0o40755)
MOBILE_OWNED = data_owner.Owner(501, 20, 0o40755)


class Log:
    def __call__(self, *parts: object) -> None:
        pass

    quiet = __call__


class DataOwnerTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.data = Path(self.tmp.name)
        self.mounted = mock.patch.object(
            data_owner.cards, 'mounted', side_effect=lambda *_, **__: contextlib.nullcontext(self.data)
        ).start()
        self.run = mock.patch.object(data_owner.host, 'run').start()
        self.addCleanup(mock.patch.stopall)

    def tearDown(self):
        self.tmp.cleanup()

    def test_the_kernel_takes_only_root_wheel_without_group_or_other_write(self):
        self.assertTrue(ROOT_OWNED.kernel_takes_updates)
        for owner in (MOBILE_OWNED, data_owner.Owner(0, 20, 0o40755), data_owner.Owner(0, 0, 0o40775)):
            self.assertFalse(owner.kernel_takes_updates, owner)

    def test_the_check_reads_switch_with_owners_read_only(self):
        (self.data / 'Switch/pretranslated').mkdir(parents=True)
        with mock.patch.object(data_owner, '_owner', side_effect=[MOBILE_OWNED, ROOT_OWNED]) as owner:
            self.assertEqual(data_owner.read('disk9s5', Log()), MOBILE_OWNED)
        switch = self.data / 'Switch'
        self.assertEqual(owner.call_args_list, [mock.call(switch), mock.call(switch / 'pretranslated')])
        self.mounted.assert_called_with('disk9s5', mock.ANY, read_only=True, owners=True)
        self.run.assert_not_called()

    def test_a_mount_that_does_not_show_the_tree_as_roots_refuses(self):
        (self.data / 'Switch/pretranslated').mkdir(parents=True)
        for tree in (MOBILE_OWNED, None):
            with (
                self.subTest(tree=tree),
                mock.patch.object(data_owner, '_owner', side_effect=[ROOT_OWNED, tree]),
                self.assertRaisesRegex(cards.RefusedError, 'owners are not honoured'),
            ):
                data_owner.read('disk9s5', Log())

    def test_a_file_where_switch_goes_refuses(self):
        (self.data / 'Switch').write_text('')
        with self.assertRaises(cards.RefusedError):
            data_owner.read('disk9s5', Log())

    def test_a_root_owned_switch_needs_no_sudo(self):
        (self.data / 'Switch').mkdir()
        with mock.patch.object(data_owner, '_owner', return_value=ROOT_OWNED):
            data_owner.ensure('disk9s5', Log())
        self.run.assert_not_called()

    def test_another_owner_is_set_under_sudo_and_read_again(self):
        (self.data / 'Switch').mkdir()
        with mock.patch.object(data_owner, '_owner', side_effect=[MOBILE_OWNED, ROOT_OWNED]):
            data_owner.ensure('disk9s5', Log())
        self.mounted.assert_called_with('disk9s5', mock.ANY, owners=True)
        folder = self.data / 'Switch'
        self.assertEqual(
            self.run.call_args_list,
            [
                mock.call(data_owner.SUDO, '--', data_owner.CHOWN, '0:0', folder, log=mock.ANY),
                mock.call(data_owner.SUDO, '--', data_owner.CHMOD, 'go-w', folder, log=mock.ANY),
            ],
        )

    def test_an_owner_that_does_not_take_fails_the_deploy(self):
        (self.data / 'Switch').mkdir()
        with (
            mock.patch.object(data_owner, '_owner', side_effect=[MOBILE_OWNED, MOBILE_OWNED]),
            self.assertRaisesRegex(host.HostError, 'still 501:20'),
        ):
            data_owner.ensure('disk9s5', Log())

    def test_sudo_is_asked_for_before_writing_only_when_switch_will_need_it(self):
        data_owner.authorize(ROOT_OWNED, Log())
        self.run.assert_not_called()
        for owner in (MOBILE_OWNED, None):
            self.run.reset_mock()
            data_owner.authorize(owner, Log())
            self.run.assert_called_once_with(data_owner.SUDO, '-v', log=mock.ANY)

    def test_the_plan_says_whether_sudo_is_needed(self):
        self.assertNotIn('sudo', data_owner.planned(ROOT_OWNED))
        self.assertIn('sudo', data_owner.planned(MOBILE_OWNED))
        self.assertIn('made by this deploy', data_owner.planned(None))


if __name__ == '__main__':
    unittest.main()
