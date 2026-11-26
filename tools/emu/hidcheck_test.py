"""hidcheck.py: the keys in a keyboard's pcap, each HID path's steps, and the judgement of a finished run."""

import struct
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock

from tools.emu import hidcheck, usbcheck


def pcap(reports: list[bytes]) -> bytes:
    """Return a QEMU USB pcap of interrupt IN completions carrying `reports`, as usbcheck reads them."""
    out = b'\0' * usbcheck.PCAP_HEADER
    for data in reports:
        packet = usbcheck.USBMON.pack(
            0, ord('C'), usbcheck.INTERRUPT, 0x81, 1, 0, 0, 0, 0, 0, 0, len(data), len(data), 0, 0, 0, 0, 0
        )
        record = packet + data
        out += struct.pack('<IIII', 0, 0, len(record), len(record)) + record
    return out


def report(*usages: int) -> bytes:
    return bytes([0, 0, *usages, *([0] * (6 - len(usages)))])


class Keys(unittest.TestCase):
    def test_each_press_counts_once_however_long_it_is_held(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'k.pcap'
            path.write_bytes(pcap([report(), report(0x0B), report(0x0B), report(), report(0x0C), report()]))
            self.assertEqual(hidcheck.keys_in(path), [0x0B, 0x0C])

    def test_no_pcap_is_no_keys(self):
        self.assertEqual(hidcheck.keys_in(Path('/nonexistent/k.pcap')), [])


class Judge(unittest.TestCase):
    LOG = (
        '[ 9.0] SwitchStall: SpringBoard[27] main thread',
        (
            '[12.0] AppleUSB20XHCITypeCPort@00500000: AppleUSBHostPort::enumerateDeviceComplete_block_invoke: '
            'enumerated 0x0627/0001/0000 (QEMU USB Keyboard / 1) at 480 Mbps'
        ),
        '[12.1] IOUSBHostHIDDevice:0x1000002f1 start',
        '[12.2] IOUSBHostHIDDevice:0x1000002f1 open by IOHIDEventDriver:0x1000002f5 (0x0)',
    )

    def judge(self, reports: list[bytes]) -> tuple[list[str], list[str]]:
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / hidcheck.PCAP).write_bytes(pcap(reports))
            return hidcheck.Typing('ios17', base=1, sent=2, owner=hidcheck.PATHS['ios17'][0]).judge(self.LOG, Path(d))

    def test_the_keys_sent_reported_in_order_pass(self):
        problems, notes = self.judge([report(0x0B), report(), report(0x0C), report()])
        self.assertEqual(problems, [])
        self.assertIn('path: kernel driver (IOUSBHostHIDDevice, IOHIDEventDriver)', notes)

    def test_a_missing_or_wrong_key_fails(self):
        problems, _ = self.judge([report(0x0B), report()])
        self.assertTrue(problems and 'reported' in problems[0])
        problems, _ = self.judge([report(0x0C), report(), report(0x0B)])
        self.assertTrue(problems and 'reported' in problems[0])

    def test_waits_for_class_attach_and_both_key_reports(self):
        with tempfile.TemporaryDirectory() as d:
            run, monitor = Path(d), Mock()
            watch = hidcheck.Typing('ios17', base=1, phase='hid')
            self.assertFalse(watch.step(self.LOG[:2], monitor, run))
            monitor.command.assert_not_called()
            self.assertFalse(watch.step(self.LOG, monitor, run))
            self.assertEqual(monitor.command.call_count, 1)
            (run / hidcheck.PCAP).write_bytes(pcap([report(0x0B), report()]))
            self.assertFalse(watch.step(self.LOG, monitor, run))
            self.assertEqual(monitor.command.call_count, 2)
            (run / hidcheck.PCAP).write_bytes(pcap([report(0x0B), report(), report(0x0C)]))
            self.assertTrue(watch.step(self.LOG, monitor, run))
            self.assertEqual(monitor.command.call_count, 2)

    def test_the_dext_path_needs_each_of_its_steps(self):
        (path,) = hidcheck.PATHS['ios27']
        lines = [
            (
                'DK: AppleUserUSBHostHIDDevice-0x10000047d waiting for server '
                'com.apple.driverkit.AppleUserHIDDrivers-100000474'
            ),
            'DK: AppleUserUSBHostHIDDevice-0x10000047d server launched, validating',
            'DK: AppleUserUSBHostHIDDevice-0x10000047d::start(IOUSBHostInterface-0x10000047c) ok',
            'AppleUserUSBHostHIDDevice:0x10000047d open by AppleUserHIDEventDriver:0x10000048a (0x0)',
        ]
        self.assertTrue(all(hidcheck.first(lines, pattern) for _, pattern in path.steps))
        self.assertIsNone(hidcheck.first(lines[1:], path.steps[0][1]))
        self.assertIsNone(hidcheck.first(self.LOG, path.steps[-1][1]))

    def test_17_native_owner_opens_the_keyboard_and_starts_on_its_hid_interface(self):
        with tempfile.TemporaryDirectory() as d:
            run, monitor = Path(d), Mock()
            watch = hidcheck.Typing('ios17', base=1, phase='hid')
            logs = [
                *self.LOG[:2],
                'DK: AppleUserUSBHostHIDDevice-0x10000030a::start(IOUSBHostInterface-0x1000002fb) ok',
            ]
            self.assertFalse(watch.step(logs, monitor, run))
            monitor.command.assert_not_called()
            logs.append('AppleUserUSBHostHIDDevice:0x10000030a open by AppleUserHIDEventDriver:0x10000036d (0x0)')
            self.assertFalse(watch.step(logs, monitor, run))
            monitor.command.assert_not_called()
            logs.append('DK: AppleUserHIDEventDriver-0x10000036d::start(IOHIDInterface-0x10000036c) ok')
            self.assertFalse(watch.step(logs, monitor, run))
            self.assertIs(watch.owner, hidcheck.PATHS['ios17'][1])
            self.assertEqual(monitor.command.call_count, 1)


if __name__ == '__main__':
    unittest.main()
