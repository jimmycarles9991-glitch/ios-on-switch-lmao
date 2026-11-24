"""battery.py's checks over boot 2's dasd and powerd lines (22-23 Nov, iOS 27)."""

import unittest

from tools.devicerun import devicerun
from tools.devicerun.battery import BATTERY
from tools.devicerun.rules import FAIL, PASS

UNMATCHED = 'No match for AppleSmartBatteryPack IOService'
NOT_FOUND = 'Failed to find battery pack with ID 0'
SCORED = 'com.apple.healthd.sync.cloud.periodic:55C469:['


def record(message: str, process: str, second: int = 0) -> dict:
    return {
        'timestamp': f'2026-11-23 00:14:{second:02d}.000000+0000',
        'bootUUID': 'B2',
        'processImagePath': process,
        'subsystem': 'com.apple.duetactivityscheduler',
        'category': '',
        'messageType': 'Default',
        'eventMessage': message,
    }


DASD = '/usr/libexec/dasd'
POWERD = '/System/Library/CoreServices/powerd.bundle/powerd'


def results(*records: dict) -> dict:
    found, _ = devicerun.run_checks(list(records), BATTERY)
    return {r.check.name: r for r in found}


class Battery(unittest.TestCase):
    def test_boot_2_had_no_pack(self):
        got = results(record(UNMATCHED, DASD), record(UNMATCHED, DASD, 1), record(NOT_FOUND, POWERD, 2))
        self.assertEqual(got['battery.pack-unmatched'].verdict, FAIL)
        self.assertEqual(got['battery.pack-not-found'].verdict, FAIL)

    def test_a_published_pack_passes(self):
        got = results(record(SCORED, DASD))
        self.assertEqual(got['battery.pack-unmatched'].verdict, PASS)
        self.assertEqual(got['battery.pack-not-found'].verdict, PASS)


if __name__ == '__main__':
    unittest.main()
