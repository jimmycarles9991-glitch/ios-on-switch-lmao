import contextlib
import io
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools.common import sudo

SUDO = {'SUDO_UID': '501', 'SUDO_GID': '20'}


class Owners(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name)

    def tearDown(self):
        self.dir.cleanup()

    def test_only_a_run_as_root_under_sudo_gives_anything_back(self):
        with mock.patch.dict(os.environ, SUDO), mock.patch.object(sudo.os, 'geteuid', return_value=0):
            self.assertEqual(sudo.user(), (501, 20))
        with mock.patch.dict(os.environ, SUDO), mock.patch.object(sudo.os, 'geteuid', return_value=501):
            self.assertIsNone(sudo.user())
        with mock.patch.dict(os.environ, clear=True), mock.patch.object(sudo.os, 'geteuid', return_value=0):
            self.assertIsNone(sudo.user())
        with mock.patch.object(sudo.os, 'chown') as chown, mock.patch.dict(os.environ, clear=True):
            sudo.give_back(self.root)
        chown.assert_not_called()

    def test_every_directory_made_goes_to_the_user(self):
        (self.root / 'had').mkdir()
        wanted = self.root / 'had/new/deeper'
        with (
            mock.patch.dict(os.environ, SUDO),
            mock.patch.object(sudo.os, 'geteuid', return_value=0),
            mock.patch.object(sudo.os, 'chown') as chown,
        ):
            self.assertEqual(sudo.user_directory(wanted), wanted)
        self.assertTrue(wanted.is_dir())
        given = [c.args[0] for c in chown.call_args_list]
        self.assertEqual(given, [wanted, wanted.parent])
        self.assertTrue(all(c.args[1:] == (501, 20) for c in chown.call_args_list))


class Rerun(unittest.TestCase):
    def test_runs_the_script_again_under_sudo(self):
        with (
            mock.patch.object(sudo.os, 'execvp', side_effect=SystemExit) as execvp,
            contextlib.redirect_stdout(io.StringIO()) as printed,
            self.assertRaises(SystemExit),
        ):
            sudo.rerun('tool.py', ['disk4', '--yes'])
        self.assertEqual(
            execvp.call_args.args,
            ('sudo', ['sudo', '--', sys.executable, os.path.abspath('tool.py'), 'disk4', '--yes']),
        )
        self.assertIn('running under sudo:', printed.getvalue())


if __name__ == '__main__':
    unittest.main()
