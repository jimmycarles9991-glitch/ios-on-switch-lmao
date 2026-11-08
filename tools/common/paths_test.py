import os
import re
import unittest
from pathlib import Path
from unittest import mock

from tools.common import paths


class Located(unittest.TestCase):
    def test_a_runfile_becomes_its_absolute_path(self):
        found = paths.located('_main/tools/common/paths_test.py')
        self.assertTrue(os.path.isabs(found))
        self.assertEqual(Path(found).read_bytes(), Path(__file__).read_bytes())

    def test_an_absolute_path_and_nothing_stay(self):
        self.assertEqual(paths.located('/usr/bin/../bin/true'), '/usr/bin/true')
        self.assertEqual(paths.located(Path('/tmp')), '/tmp')
        self.assertIsNone(paths.located(None))
        self.assertEqual(paths.located(''), '')


class Places(unittest.TestCase):
    def test_workspace_is_bazel_runs_or_this_checkout(self):
        with mock.patch.dict(os.environ, {'BUILD_WORKSPACE_DIRECTORY': '/src/tree'}):
            self.assertEqual(paths.workspace(), Path('/src/tree'))
        with mock.patch.dict(os.environ, clear=True):
            self.assertTrue((paths.workspace() / 'tools/common/paths.py').is_file())

    def test_state_dir_follows_switch_home(self):
        with mock.patch.dict(os.environ, {'SWITCH_HOME': '/elsewhere'}):
            self.assertEqual(paths.state_dir(), Path('/elsewhere'))
        with mock.patch.dict(os.environ, {'SWITCH_HOME': ''}):
            self.assertEqual(paths.state_dir(), Path.home() / 'Library/Application Support/switch-ios')

    def test_run_log_names_the_run_and_its_time(self):
        log = paths.run_log('direct')
        self.assertEqual(log.parent, Path.home() / 'Library/Logs/switch-ios')
        self.assertRegex(log.name, re.compile(r'direct-\d{8}-\d{6}\.log'))
        self.assertEqual(paths.run_log('deploy', 'switch-ios-install').parent.name, 'switch-ios-install')


if __name__ == '__main__':
    unittest.main()
