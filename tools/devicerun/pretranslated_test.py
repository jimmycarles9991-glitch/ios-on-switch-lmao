"""pretranslated.py's checks over the kernel's deploy-update lines."""

import unittest

from tools.devicerun import devicerun, reports
from tools.devicerun.pretranslated import PRETRANSLATED
from tools.devicerun.rules import FAIL, INFO, NO_DATA, PASS

KERNEL = '/System/Library/Kernels/kernel.development'


def kernel(message: str, second: int = 0) -> dict:
    return {
        'timestamp': f'2026-11-26 10:00:{second:02d}.000000+0000',
        'bootUUID': 'B',
        'processImagePath': KERNEL,
        'subsystem': '',
        'category': '',
        'messageType': 'Default',
        'eventMessage': message,
    }


CAPTURED = 'SwitchPretranslated: 1830 files from /private/var/Switch/pretranslated (fsid 1:2)'


def results(*records: dict, captured: bool = True) -> dict:
    found, _ = devicerun.run_checks([kernel(CAPTURED)] * captured + list(records), PRETRANSLATED)
    return {r.check.name: r for r in found}


class Pretranslated(unittest.TestCase):
    def test_an_update_taken_passes(self):
        got = results(kernel("SwitchPretranslated: /usr/libexec/locationd from the deploy's update"))
        for name, result in got.items():
            self.assertEqual(result.verdict, INFO if name == 'pretranslated.capture' else PASS, name)

    def test_any_other_line_for_a_file_fails(self):
        for line in (
            (
                "SwitchPretranslated: /usr/libexec/locationd not updated (not signed as its CDHash); Apple's file "
                "runs translated at load time, without this build's edit of it"
            ),
            'SwitchPretranslated: /usr/libexec/locationd not updated (not in the tree)',
            "SwitchPretranslated: /usr/libexec/locationd from the deploy's update, eventually",
        ):
            with self.subTest(line=line):
                self.assertEqual(results(kernel(line))['pretranslated.update'].verdict, FAIL)
        self.assertEqual(results()['pretranslated.update'].verdict, PASS, "the tree's own line names no update")

    def test_a_log_without_the_capture_line_is_no_data(self):
        line = kernel('SwitchPretranslated: /usr/libexec/locationd not updated (not in the tree)')
        got = results(line, captured=False)
        for name in ('pretranslated.update', 'pretranslated.update-folder'):
            self.assertEqual(got[name].verdict, NO_DATA, name)

    def test_an_unusable_update_folder_fails_unless_it_is_only_missing(self):
        missing = (
            'SwitchPretranslated: 1 of 1 updates needed, none applied (/private/var/Switch/pretranslated-updates: '
        )
        self.assertEqual(results(kernel(missing + 'no such file)'))['pretranslated.update-folder'].verdict, PASS)
        owner = (
            'SwitchPretranslated: 1 of 1 updates needed, none applied (/private/var/Switch: not root:wheel, or '
            'writable by group or others)'
        )
        self.assertEqual(results(kernel(owner))['pretranslated.update-folder'].verdict, FAIL)
        self.assertEqual(results(kernel(missing + 'unreadable)'))['pretranslated.update-folder'].verdict, FAIL)

    def test_a_tree_turned_off_fails_and_an_unreadable_update_list_turns_it_off(self):
        self.assertEqual(results()['pretranslated.off'].verdict, PASS)
        off = kernel('SwitchPretranslated: off (/chosen update list: 6); the System files run translated at load time')
        got = results(off, captured=False)
        self.assertEqual(got['pretranslated.off'].verdict, FAIL)
        self.assertIn('/chosen update list', got['pretranslated.off'].observed)

    def test_a_turned_down_update_blocks_the_build(self):
        line = kernel('SwitchPretranslated: /usr/libexec/locationd not updated (update file)')
        found, _ = devicerun.run_checks([kernel(CAPTURED), line], PRETRANSLATED)
        top = reports.blocking([('pretranslated', found)])
        self.assertIn('Blocks the build: pretranslated.update', top)
        found, _ = devicerun.run_checks([kernel(CAPTURED)], PRETRANSLATED)
        self.assertEqual(reports.blocking([('pretranslated', found)]), 'Blocks the build: nothing\n')


if __name__ == '__main__':
    unittest.main()
