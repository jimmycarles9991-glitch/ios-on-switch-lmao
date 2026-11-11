"""protection.py's checks over boot 15's lines (11-08, iOS 27) and a kernel that labels."""

import unittest

from tools.devicerun import devicerun
from tools.devicerun.protection import PROTECTION
from tools.devicerun.rules import FAIL, NA, PASS

ON = 'SwitchProtection: on, files before id 412345 read as class D'
SQLITE = 'volume does not support data protection, stripping SQLITE_OPEN_FILEPROTECTION_* flags'
UNFORMATTED = '#dataprotection init, content protection formatting, 0'
FORMATTED = '#dataprotection init, content protection formatting, 1'
SPOTLIGHT = (
    'SpotlightKnowledgeDaemon/UpdaterDaemon.swift:978: Fatal error: DocumentProcessing directory has unexpected '
    'protection class NSURLFileProtectionNone.'
)
LOCATIOND_EXIT = 'SwitchCrash: locationd[72] exit reason namespace 2 code 0xa'
LOCATIOND_REGS = 'SwitchCrash: locationd[72] signal 10 exception 1 code 0x2 subcode 0x1e628d874'


def record(process: str, message: str, second: int = 0) -> dict:
    path = 'kernel.development' if process == 'kernel' else '/usr/libexec/' + process
    return {
        'timestamp': f'2026-11-08 19:07:{second:02d}.000000-0800',
        'bootUUID': '31AB01C1',
        'processImagePath': path,
        'subsystem': '',
        'category': '',
        'messageType': 'Default',
        'eventMessage': message,
    }


def results(*records: dict) -> dict:
    found, _ = devicerun.run_checks(list(records), PROTECTION)
    return {r.check.name: r for r in found}


class Protection(unittest.TestCase):
    def test_boot_15_lines_fail_once_the_kernel_labels(self):
        got = results(
            record('kernel', ON),
            record('PosterBoard', SQLITE, 1),
            record('locationd', UNFORMATTED, 2),
            record('spotlightknowledged.updater', SPOTLIGHT, 3),
            record('kernel', LOCATIOND_EXIT, 4),
            record('kernel', LOCATIOND_REGS, 4),
        )
        self.assertEqual(got['protection.sqlite'].verdict, FAIL)
        self.assertEqual(got['protection.locationd'].verdict, FAIL)
        self.assertEqual(got['protection.spotlight'].verdict, FAIL)
        self.assertEqual(got['protection.crashes'].verdict, FAIL)
        self.assertIn('1 lines', got['protection.crashes'].observed)
        self.assertEqual(got['protection.off'].verdict, PASS)

    def test_a_labelling_boot_passes(self):
        got = results(record('kernel', ON), record('locationd', FORMATTED, 1))
        for name in ('protection.sqlite', 'protection.locationd', 'protection.spotlight', 'protection.crashes'):
            self.assertEqual(got[name].verdict, PASS, name)

    def test_without_the_kernel_line_the_volume_checks_do_not_apply(self):
        got = results(record('PosterBoard', SQLITE), record('locationd', UNFORMATTED, 1))
        self.assertEqual(got['protection.sqlite'].verdict, NA)
        self.assertEqual(got['protection.locationd'].verdict, NA)


if __name__ == '__main__':
    unittest.main()
