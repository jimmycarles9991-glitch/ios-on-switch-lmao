"""joycon.py's checks over boot 18's Joy-Con lines (11-09, iOS 27), in the rail's detach format with the address."""

import unittest

from tools.devicerun import devicerun
from tools.devicerun.joycon import JOYCON
from tools.devicerun.rules import FAIL, NO_DATA, PASS

KERNEL = '/System/Library/Caches/com.apple.kernelcaches/kernel.development'
BACKBOARDD = '/usr/libexec/backboardd'
RIGHT, LEFT = '48:f1:eb:b6:9e:d1', '48:f1:eb:b6:cf:96'


def record(time: str, message: str, process: str = KERNEL, subsystem: str = '') -> dict:
    return {
        'timestamp': f'2026-11-09 {time}000-0800',
        'bootUUID': '18B00718',
        'processImagePath': process,
        'subsystem': subsystem,
        'category': '',
        'messageType': 'Default',
        'eventMessage': message,
    }


def detached(time: str, side: str, address: str, ms: int, paired: bool = True) -> dict:
    state = 'paired as its session opened' if paired else 'not paired'
    return record(
        time,
        f'SwitchJoyCon: Joy-Con ({side}) {address} ({state}) on the rail stopped answering after 24078 input reports: '
        f'100 requests unanswered over {ms} ms (line errors 0, breaks 0 since its last reply; line errors 0, ring '
        f'overflows 0 since the handshake; RX DMA on: 24323 takes, 0 found it behind, 2 tails at the tick)',
    )


def paged(time: str, address: str) -> dict:
    return record(time, f'SwitchBluetooth: link <- Connection Request {address} class 000508 type 1')


def device_info(time: str, side: str) -> dict:
    return record(
        time,
        f"Device is a 'Joy-Con ({side})' with f/w version 0x2004.",
        BACKBOARDD,
        'com.apple.GameController.HID',
    )


BOOT18 = [
    detached('16:51:57.945', 'R', RIGHT, 1602),
    paged('16:52:06.163', RIGHT),
    device_info('16:52:07.187', 'R'),
    detached('16:52:07.562', 'R', RIGHT, 1585),
    detached('16:52:41.732', 'L', LEFT, 1602),
    paged('16:52:48.606', LEFT),
    device_info('16:52:49.248', 'L'),
    detached('16:52:49.710', 'L', LEFT, 1587),
]


def results(*records: dict) -> dict:
    found, _ = devicerun.run_checks(list(records), JOYCON)
    return {r.check.name: r for r in found}


class Paging(unittest.TestCase):
    def test_boot_18_fails_on_the_detaches_nothing_paged_after(self):
        got = results(*BOOT18)['joycon.paged-on-detach']
        self.assertEqual(got.verdict, FAIL)
        self.assertIn('4 lines, 2 without a joycon.bt-paged line within 3 s', got.observed)
        self.assertIn(f'16:51:57 {RIGHT}', got.observed)
        self.assertIn(f'16:52:41 {LEFT}', got.observed)

    def test_a_page_within_3_s_of_the_last_reply_passes(self):
        fixed = [detached('16:51:57.945', 'R', RIGHT, 1602), paged('16:51:57.100', RIGHT), *BOOT18[1:4]]
        self.assertEqual(results(*fixed)['joycon.paged-on-detach'].verdict, PASS)

    def test_another_joycons_page_or_a_late_one_does_not_count(self):
        self.assertEqual(
            results(detached('16:52:07.562', 'R', RIGHT, 1585), paged('16:52:06.163', LEFT))[
                'joycon.paged-on-detach'
            ].verdict,
            FAIL,
        )
        self.assertEqual(
            results(detached('16:52:07.562', 'R', RIGHT, 1585), paged('16:52:09.100', RIGHT))[
                'joycon.paged-on-detach'
            ].verdict,
            FAIL,
        )

    def test_an_unpaired_joycons_detach_is_not_judged(self):
        got = results(detached('16:51:57.945', 'R', RIGHT, 1602, paired=False))
        self.assertEqual(got['joycon.paged-on-detach'].verdict, NO_DATA)
        self.assertIn('R/not paired x1', got['joycon.detached'].observed)

    def test_bluetooth_input_reaches_the_plugin(self):
        got = results(*BOOT18)
        self.assertEqual(got['joycon.bt-input'].verdict, PASS)
        self.assertEqual(got['joycon.bt-paged'].verdict, PASS)
        silent = results(paged('16:52:06.163', RIGHT), device_info('16:52:09.300', 'R'))
        self.assertEqual(silent['joycon.bt-input'].verdict, FAIL)


if __name__ == '__main__':
    unittest.main()
