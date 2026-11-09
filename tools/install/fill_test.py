"""The os delta's entries Data's /private/var and the pretranslated tree take."""

import unittest

from tools.install import fill


def entry(path: str) -> dict:
    return {'path': path, 'file': 'files/0', 'sha256_in': 'in', 'sha256_out': 'out'}


class DataEntriesTest(unittest.TestCase):
    def test_var_files_are_rewritten_relative_to_data(self):
        self.assertEqual([e['path'] for e in fill.data_entries([entry('/private/var/db/tool')])], ['/db/tool'])

    def test_system_files_stay_out(self):
        self.assertEqual(fill.data_entries([entry('/usr/libexec/tool')]), [])

    def test_staged_system_apps_keep_apples_bytes(self):
        staged = [
            entry('/private/var/staged_system_apps/FaceTime.app/FaceTime'),
            entry('/private/var/staged_system_apps/Books.app/PlugIns/Spotlight.appex/Spotlight'),
        ]
        self.assertEqual(fill.data_entries(staged), [])

    def test_the_tree_takes_system_files_and_staged_apps_only(self):
        entries = [
            entry('/usr/libexec/tool'),
            entry('/private/var/db/tool'),
            entry('/private/var/staged_system_apps/FaceTime.app/FaceTime'),
        ]
        self.assertEqual(
            [e['path'] for e in fill.tree_entries(entries)],
            ['/usr/libexec/tool', '/private/var/staged_system_apps/FaceTime.app/FaceTime'],
        )

    def test_a_sibling_named_like_the_staged_folder_is_still_rewritten(self):
        self.assertEqual(len(fill.data_entries([entry('/private/var/staged_system_apps.old/tool')])), 1)


if __name__ == '__main__':
    unittest.main()
