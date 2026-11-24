"""motion.py's checks over boot 2's CoreMotion lines (22-23 Nov, iOS 27)."""

import unittest

from tools.devicerun import devicerun
from tools.devicerun.motion import MOTION
from tools.devicerun.rules import FAIL, INFO, PASS

LOCATIOND = '/usr/libexec/locationd'
BACKBOARDD = '/usr/libexec/backboardd'
SPRINGBOARD = '/System/Library/CoreServices/SpringBoard.app/SpringBoard'
HWTEST = '/private/var/containers/Bundle/Application/X/SwitchHWTest.app/SwitchHWTest'


def record(message: str, process: str, second: int = 0) -> dict:
    return {
        'timestamp': f'2026-11-22 23:53:{second:02d}.000000+0000',
        'bootUUID': 'B2',
        'processImagePath': process,
        'subsystem': 'com.apple.locationd.Motion',
        'category': 'Motion',
        'messageType': 'Default',
        'eventMessage': message,
    }


def results(*records: dict) -> dict:
    found, _ = devicerun.run_checks(list(records), MOTION)
    return {r.check.name: r for r in found}


BOOT_2 = (
    record('[CLIoHidInterface] Adding new Device with usage pair {65292, 26}', BACKBOARDD, 2),
    record('[CLIoHidInterface] Service for device with usage pair {65292, 26} is not ready', BACKBOARDD, 3),
    record('[CLIoHidInterface] Could not set all properties after too many retries. Giving up.', BACKBOARDD, 4),
    record('[CLIoHidInterface] Service for usage pair {65292, 5} is not ready', HWTEST, 5),
    record('[FastPath] Failed to open because of null service ref', SPRINGBOARD, 6),
    record('[CLIoHidInterface] Could not set all properties after too many retries. Giving up.', SPRINGBOARD, 7),
    record('[CLIoHidInterface] Adding new Device with usage pair {65292, 8}', LOCATIOND, 8),
)


class Motion(unittest.TestCase):
    def test_boot_2_asked_for_the_coprocessor(self):
        got = results(*BOOT_2)
        for name in (
            'motion.spu-added',
            'motion.spu-not-ready',
            'motion.give-up.backboardd',
            'motion.give-up.SpringBoard',
            'motion.fastpath-null',
        ):
            self.assertEqual(got[name].verdict, FAIL, name)
        self.assertEqual(got['motion.spu-added-ungated'].verdict, INFO)

    def test_locationds_own_copy_is_not_judged_here(self):
        got = results(
            record('[CLIoHidInterface] Adding new Device with usage pair {65292, 1}', LOCATIOND),
            record('[CLIoHidInterface] Service for device with usage pair {65292, 1} is not ready', LOCATIOND, 1),
            record('[CLIoHidInterface] Could not set all properties after too many retries. Giving up.', LOCATIOND, 2),
        )
        self.assertEqual(got['motion.spu-added'].verdict, PASS)
        self.assertEqual(got['motion.spu-not-ready'].verdict, PASS)
        self.assertEqual(got['motion.locationd-spu'].verdict, INFO)
        self.assertEqual(len(got['motion.locationd-spu'].matches), 3)

    def test_the_accelerometer_and_gyroscope_alone_pass(self):
        got = results(
            record('[CLIoHidInterface] Adding new Device with usage pair {65280, 3}', SPRINGBOARD),
            record(
                '[CLIoHidInterface] Property for usage pair {65280, 9}: {ReportInterval = 10000} was set', HWTEST, 1
            ),
        )
        for name, result in got.items():
            if name not in ('motion.spu-added-ungated', 'motion.locationd-spu'):
                self.assertEqual(result.verdict, PASS, name)


if __name__ == '__main__':
    unittest.main()
