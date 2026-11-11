"""apps.py's checks over boot 16 and 16b's lsd, SpringBoard, backboardd and kernel lines (11-09, iOS 27)."""

import unittest

from tools.devicerun import devicerun
from tools.devicerun.apps import APPS
from tools.devicerun.rules import FAIL, NO_DATA, PASS

FIRST_BOOT = 'Reboot detected, performing first boot work.'
REMOVED = 'Marking container at /Applications/ for removal'
NOT_INSTALLED = (
    'bad app layout found in persistent storage. one of the referenced apps is not installed.  appLayout=<SBAppLayout: '
    '0x8abb3e4c0; primary: com.brunostjohn.switch.hwtest:default; environment: main> {'
)
APPLE_NOT_INSTALLED = NOT_INSTALLED.replace('com.brunostjohn.switch.hwtest', 'com.apple.Maps')


REQUESTED = 'SwitchRawBoot: data migration requested (staged system apps)'
MIGRATED = 'Data migrator completed with success: NO!'
NOT_NEEDED = 'Data migrator was not necessary'
COMPLETE = 'Migration complete (if performed). (Elapsed time: {} seconds)'
REGISTERED = 'SwitchAppRegistration: Register com.brunostjohn.switch.hwtest (/Applications/SwitchHWTest.app)'
FAILED = (
    'SwitchAppRegistration: Register com.brunostjohn.switch.hwtest (/Applications/SwitchHWTest.app) failed: '
    'Error Domain=NSCocoaErrorDomain Code=4099'
)


def record(process: str, message: str, second: int = 0, boot: str = '88A60631', level: str = 'Default') -> dict:
    return {
        'timestamp': f'2026-11-09 03:48:{second:02d}.000000-0800',
        'bootUUID': boot,
        'processImagePath': f'/usr/libexec/{process}',
        'subsystem': 'com.apple.launchservices',
        'category': 'default',
        'messageType': level,
        'eventMessage': message,
    }


def results(*records: dict) -> dict:
    found, _ = devicerun.run_checks(list(records), APPS)
    return {r.check.name: r for r in found}


class OverlayApps(unittest.TestCase):
    def test_boot_16b_loses_the_overlay_apps(self):
        got = results(record('lsd', FIRST_BOOT), record('lsd', REMOVED, 3), record('SpringBoard', NOT_INSTALLED, 54))
        self.assertEqual(got['apps.overlay-container-removed'].verdict, FAIL)
        self.assertEqual(got['apps.overlay-not-installed'].verdict, FAIL)
        self.assertIn('com.brunostjohn.switch.hwtest', got['apps.overlay-not-installed'].observed)

    def test_a_boot_that_keeps_them_passes(self):
        got = results(record('lsd', FIRST_BOOT), record('SpringBoard', APPLE_NOT_INSTALLED, 54))
        self.assertEqual(got['apps.overlay-container-removed'].verdict, PASS)
        self.assertEqual(got['apps.overlay-not-installed'].verdict, PASS)

    def test_no_first_boot_work_is_no_data(self):
        self.assertEqual(results()['apps.overlay-container-removed'].verdict, NO_DATA)


class Migration(unittest.TestCase):
    def test_boot_16_migrated_because_the_kernel_asked(self):
        got = results(
            record('kernel', REQUESTED, 1, boot='A'),
            record('backboardd', MIGRATED, 40, boot='A'),
            record('backboardd', COMPLETE.format('166.97'), 41, boot='A'),
        )
        self.assertEqual(got['apps.unrequested-migration'].verdict, PASS)

    def test_a_migration_nobody_asked_for_fails(self):
        got = results(
            record('kernel', REQUESTED, 1, boot='A'),
            record('backboardd', MIGRATED, 40, boot='A'),
            record('backboardd', COMPLETE.format('166.97'), 41, boot='A'),
            record('backboardd', MIGRATED, 50, boot='B'),
            record('backboardd', COMPLETE.format('120.00'), 51, boot='B'),
        )
        self.assertEqual(got['apps.unrequested-migration'].verdict, FAIL)
        self.assertIn('1 lines', got['apps.unrequested-migration'].observed)

    def test_a_boot_without_a_migration_passes(self):
        got = results(record('backboardd', NOT_NEEDED, 1), record('backboardd', COMPLETE.format('0.10'), 2))
        self.assertEqual(got['apps.unrequested-migration'].verdict, PASS)

    def test_no_backboardd_line_is_no_data(self):
        self.assertEqual(results()['apps.unrequested-migration'].verdict, NO_DATA)


class Registration(unittest.TestCase):
    def test_a_registration_is_reported(self):
        got = results(record('switch-appregd', REGISTERED))
        self.assertIn('Register/com.brunostjohn.switch.hwtest', got['apps.registered'].observed)
        self.assertEqual(got['apps.registration-failed'].verdict, PASS)

    def test_a_failure_fails(self):
        got = results(record('switch-appregd', FAILED, level='Error'))
        self.assertEqual(got['apps.registration-failed'].verdict, FAIL)


if __name__ == '__main__':
    unittest.main()
