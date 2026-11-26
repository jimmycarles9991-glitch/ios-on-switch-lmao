"""usbcheck.py against kernel log lines and pcaps shaped like the emulator's (usb-emu runs of 2026-11-13)."""

import struct
import tempfile
import unittest
from pathlib import Path

from tools.emu import usbcheck, usbdev

KEYBOARD_ENUMERATED = (
    '[     2.592158] AppleUSB20XHCIPort@00500000: AppleUSBHostPort::enumerateDeviceComplete_block_invoke: '
    'enumerated 0x0627/0001/0000 (QEMU USB Keyboard / 1) at 480 Mbps'
)
PORT_REFUSED = '[     3.1] SwitchUSB: USB-C port refused a USB 2 transport'
ROOT_PORT_FAILED = '[     2.4] SwitchUSB: USB-C root port 5 did not start'


def record(event: str, pipe: tuple[int, int], length: int, *, setup: bytes = b'', data: bytes = b'') -> bytes:
    transfer, endpoint = pipe
    flag_setup = 0 if setup else ord('-')
    header = usbcheck.USBMON.pack(
        1, ord(event), transfer, endpoint, 1, 1, flag_setup, 0, 0, 0, 0, length, len(data), 0, 0, 0, 0, 0
    )
    if setup:
        header = header[:40] + setup + header[48:]
    packet = header + data
    return struct.pack('<IIII', 0, 0, len(packet), len(packet)) + packet


def pcap(*records: bytes) -> bytes:
    return struct.pack('<IHHiIII', 0xA1B2C3D4, 2, 4, 0, 0, 4160, 220) + b''.join(records)


ENUMERATION = (
    record('S', (usbcheck.CONTROL, 0x80), 0, setup=bytes.fromhex('8006000100001200')),
    record('C', (usbcheck.CONTROL, 0x80), 18, data=bytes(18)),
    record('S', (usbcheck.CONTROL, 0x00), 0, setup=bytes.fromhex('0009010000000000')),
    record('C', (usbcheck.CONTROL, 0x00), 0),
)


class MilestoneTest(unittest.TestCase):
    def test_apples_enumeration_line_reaches_it(self):
        find = usbcheck.reached(['QEMU USB Keyboard'])
        self.assertEqual(find(['[ 1.0] SwitchUSB: host controller up', KEYBOARD_ENUMERATED]), KEYBOARD_ENUMERATED)

    def test_every_device_must_arrive(self):
        find = usbcheck.reached(['QEMU USB Hub', 'QEMU USB Keyboard'])
        self.assertIsNone(find([KEYBOARD_ENUMERATED]))

    def test_a_refused_transport_or_a_root_port_that_did_not_start_fails_it(self):
        self.assertEqual(usbcheck.failure(['[ 2.4] SwitchUSB: host controller up', PORT_REFUSED]), PORT_REFUSED)
        self.assertEqual(usbcheck.failure([ROOT_PORT_FAILED]), ROOT_PORT_FAILED)

    def test_an_enumerated_keyboard_does_not(self):
        self.assertIsNone(usbcheck.failure(['[ 2.4] SwitchUSB: host controller up', KEYBOARD_ENUMERATED]))


class PcapTest(unittest.TestCase):
    def setUp(self):
        self.run = Path(tempfile.mkdtemp())

    def write(self, index: int, name: str, *records: bytes) -> None:
        (self.run / usbdev.pcap(index, name)).write_bytes(pcap(*records))

    def test_requests_are_named_in_order(self):
        self.write(0, 'keyboard', *ENUMERATION)
        found = usbcheck.urbs(self.run / usbdev.pcap(0, 'keyboard'))
        self.assertEqual(usbcheck.requests(found), ['80 06 0100 0000', '00 09 0001 0000'])

    def test_an_enumerated_device_passes(self):
        self.write(0, 'storage', *ENUMERATION, record('C', (usbcheck.BULK, 0x81), 36, data=bytes(36)))
        problems, notes = usbcheck.after_run(self.run, ['storage'])
        self.assertEqual(problems, [])
        self.assertIn('bulk 0x81 x1', notes[0])

    def test_a_device_nobody_configured_fails(self):
        self.write(0, 'keyboard', *ENUMERATION[:2])
        problems, _ = usbcheck.after_run(self.run, ['keyboard'])
        self.assertEqual(problems, ['keyboard: never given a configuration'])

    def test_a_missing_pcap_is_a_device_nobody_talked_to(self):
        problems, notes = usbcheck.after_run(self.run, ['keyboard'])
        self.assertEqual(len(problems), 2)
        self.assertIn('no data moved', notes[0])

    def test_interrupter_targets_are_counted(self):
        (self.run / 'qemu-guest.log').write_text(
            '123@1.5:usb_xhci_intr_target TR_STATUS, slot 1, ep 1, interrupter target 1\n' * 2
        )
        _, notes = usbcheck.after_run(self.run, [])
        self.assertEqual(notes, ['interrupter target: TR_STATUS, slot 1, ep 1, interrupter target 1 x2'])


class RefusedAccessTest(PcapTest):
    def test_a_refused_register_access_or_smmu_fault_fails_it(self):
        self.write(0, 'keyboard', *ENUMERATION)
        (self.run / 'qemu-guest.log').write_text(
            'tegra.xusb: xHCI read at 0x44 not decoded (IPFS CONFIGURATION 0, FPCI CFG_1 0 CFG_4 0)\n'
            'tegra.xusb_host.smmu: SMMU fault on a read at 0x100000 (ASID 124)\n'
        )
        problems, _ = usbcheck.after_run(self.run, ['keyboard'])
        self.assertEqual(
            problems,
            [
                'tegra.xusb: xHCI read at 0x44 not decoded (IPFS CONFIGURATION 0, FPCI CFG_1 0 CFG_4 0)',
                'tegra.xusb_host.smmu: SMMU fault on a read at 0x100000 (ASID 124)',
            ],
        )


class PortsTest(unittest.TestCase):
    def test_devices_behind_a_hub(self):
        self.assertEqual(usbdev.ports(['hub', 'keyboard', 'storage']), ['1', '1.1', '1.2'])

    def test_two_devices_without_a_hub_are_refused(self):
        with self.assertRaises(SystemExit):
            usbdev.ports(['keyboard', 'storage'])

    def test_a_device_brings_its_partner_and_pcap(self):
        args = usbdev.qemu_args(Path('/run'), ['keyboard'])
        self.assertEqual(args[:2], ['-global', 'bm92t36.partner=sink'])
        self.assertIn('usb-kbd,bus=xusb.0,port=1,id=usb0,pcap=usb0-keyboard.pcap', args)


if __name__ == '__main__':
    unittest.main()
