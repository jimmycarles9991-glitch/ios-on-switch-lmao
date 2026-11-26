"""The usb group's DMA rows: the memory controller's XUSB faults, the mapper's errors and the development probe."""

import unittest

from tools.devicerun.rules import FAIL, INFO, NO_DATA, PASS
from tools.devicerun.usb_boot_test import FAILED_ENUMERATION, FRAGMENT, MAX_PACKET, plugged
from tools.devicerun.usb_test import DEXT, fixture, judge, record

MC_ERROR = 'TegraSMMU: MC error '
PAGE = 'INVALID_SMMU_PAGE (page readable 1 writable 0 nonsecure 1)'


def mc_fault(client: str, name: str, direction: str, address: str) -> str:
    """Return the memory controller's line for one error (TegraMCErrors.cpp, describe in TegraMCErrors.h)."""
    return (
        f'{MC_ERROR}INVALID_SMMU_PAGE: {PAGE}, a {direction} by {name} (client {client}) at {address}; INTSTATUS 0x400 '
        f'ERR_STATUS 0x6d01{client[2:]} ERR_ADR {address}'
    )


def run_over(errors: int, client: str, name: str, last: str, cause: str | None) -> str:
    """Return the line that ends a client's run past its first four errors (describeRun in TegraMCErrors.h).

    `cause` None is the line of a build before describeRun named the cause.
    """
    ended = f', ended by {cause}' if cause else ''
    described = last.removeprefix(MC_ERROR)
    printed = f'{errors} errors by {name} (client {client}), the first 4 printed{ended}'
    return f'{MC_ERROR}run over: {printed}; the last {described}'


class Dma(unittest.TestCase):
    HOSTW = ('0x4b', 'csw_xusb_hostw (XUSB host)')
    DEVW = ('0x4d', 'csw_xusb_devw (XUSB device)')
    UNMAP = (
        'SwitchUSB: DMA unmap of 0x80042000 +4096 (IOBufferMemoryDescriptor) matches no mapping: no cache maintenance '
        'or copy back done'
    )

    def test_a_host_client_fault_fails_bring_up_ahead_of_the_stale_data_it_explains(self):
        faults = [
            (0.9, mc_fault(*self.HOSTW, 'write', '0x80001000')),
            (1.5, mc_fault(*self.HOSTW, 'write', '0x80002000')),
        ]
        faults.append((1.6, mc_fault('0x4a', 'csr_xusb_hostr (XUSB host)', 'read', '0x80003000')))
        _, rows = judge(plugged(*faults, (0.95, FRAGMENT), (0.95, MAX_PACKET), (0.951, FAILED_ENUMERATION)))
        self.assertEqual(
            rows['usb.smmu.fault'].observed,
            '3 XUSB host faults: csw_xusb_hostw (0x4b): 2 errors, the first a write of 0x80001000 at 10:00:00.900, '
            f'{PAGE}; csr_xusb_hostr (0x4a): 1 error, the first a read of 0x80003000 at 10:00:01.600, {PAGE}',
        )
        self.assertTrue(rows['usb.host.first-failure'].observed.startswith('bringup; later failures: transfer-data'))
        self.assertEqual(rows['usb.smmu.fault-device'].verdict, PASS)

    def test_a_device_client_fault_is_device_modes(self):
        _, rows = judge([record(1, mc_fault(*self.DEVW, 'write', '0x90000000'))])
        self.assertEqual(rows['usb.smmu.fault-device'].verdict, FAIL)
        self.assertIn('usb.smmu.fault-device', rows['usb.layer.13-device'].observed)
        self.assertEqual(rows['usb.smmu.fault'].verdict, PASS)
        other = mc_fault('0x4b', 'csw_xusb_hostw (XUSB host)', 'read', '0x1').replace('client 0x4b', 'client 0x10')
        _, rows = judge([record(1, other)])
        self.assertEqual(rows['usb.smmu.fault'].verdict, PASS, 'not an XUSB client')
        unattached = 'TegraSMMU: MC error interrupt (INTID 109) could not be attached: memory controller errors go '
        _, rows = judge([record(1, unattached + 'unreported')])
        self.assertEqual(rows['usb.smmu.fault'].verdict, NO_DATA)

    def test_a_passing_boot_has_no_fault_and_the_unmap_line_fails_transfer_data(self):
        _, rows = fixture('emu_g6_keyboard_ios27')
        self.assertEqual(rows['usb.smmu.fault'].observed, 'no XUSB host client fault reported')
        results, rows = judge(plugged(*[(1.02 + 0.2 * i, line) for i, line in enumerate(DEXT)], (2, self.UNMAP)))
        self.assertIn('IOBufferMemoryDescriptor x1', results['usb.dma.unmap-unmatched'].observed)
        self.assertEqual(rows['usb.host.first-failure'].observed.split(';')[0], 'transfer-data')
        truncated = 'SwitchUSB: incomplete DMA bounce copy back at 0x80042000 (4096 bytes)'
        results, rows = judge(plugged(*[(1.02 + 0.2 * i, line) for i, line in enumerate(DEXT)], (2, truncated)))
        self.assertEqual(results['usb.dma.copyback-incomplete'].verdict, FAIL)
        self.assertEqual(rows['usb.host.first-failure'].observed.split(';')[0], 'transfer-data')


class McRuns(unittest.TestCase):
    """A client's run of faults: its first four lines, then one run-over line with the run's count when it ends."""

    HOSTW = ('0x4b', 'csw_xusb_hostw (XUSB host)')
    FIRST = f'csw_xusb_hostw (0x4b): {{}}, the first a write of 0x80001000 at 10:00:01.000, {PAGE}'

    def storm(self, *after: tuple[float, str]) -> dict:
        lines = [(1.0 + i, mc_fault(*self.HOSTW, 'write', f'0x8000{i + 1}000')) for i in range(4)]
        _, rows = judge([record(second, line) for second, line in sorted([*lines, *after])])
        return rows['usb.smmu.fault']

    def over(self, cause: str | None, errors: int = 37) -> str:
        return run_over(errors, *self.HOSTW, mc_fault(*self.HOSTW, 'write', '0x80009000'), cause)

    def test_the_count_and_the_cause_come_from_the_run_over_line(self):
        run = '37 XUSB host faults: ' + self.FIRST.format('37 errors') + ', a run of 37 bounded at 4 lines, over at '
        causes = {
            'displaced': "displaced (16 errors by other clients, or a ninth client's run)",
            'SC7': "at SC7's resume",
            'detach': "at its swgroup's detach, which SwitchUSB never asks for on an XUSB group",
            None: 'cause not printed (a build before the cause)',
        }
        for cause, text in causes.items():
            row = self.storm((6.0, self.over(cause)))
            self.assertEqual(row.verdict, FAIL)
            self.assertEqual(row.observed, f'{run}10:00:06.000 {text}', cause)

    def test_four_lines_and_no_run_over_line_are_at_least_four(self):
        row = self.storm()
        self.assertEqual(
            row.observed,
            'at least 4 XUSB host faults: '
            + self.FIRST.format('at least 4 errors')
            + ", a run bounded at 4 lines still open at the log's end",
        )

    def test_a_later_run_counts_on_and_a_new_open_run_is_at_least_its_lines(self):
        later = [(9.0 + i, mc_fault(*self.HOSTW, 'read', '0x8000a000')) for i in range(2)]
        row = self.storm((7.2, self.over('SC7', 20)), *later)
        self.assertTrue(row.observed.startswith('22 XUSB host faults: ' + self.FIRST.format('22 errors')))
        self.assertNotIn('still open', row.observed)
        later = [(9.0 + i, mc_fault(*self.HOSTW, 'read', '0x8000a000')) for i in range(4)]
        row = self.storm((7.2, self.over('displaced', 20)), *later)
        self.assertTrue(row.observed.startswith('at least 24 XUSB host faults: '))
        self.assertTrue(row.observed.endswith("a run bounded at 4 lines still open at the log's end"))

    def test_three_lines_are_three_errors_with_no_run(self):
        lines = [record(1.0 + i, mc_fault(*self.HOSTW, 'write', '0x80001000')) for i in range(3)]
        _, rows = judge(lines)
        self.assertEqual(rows['usb.smmu.fault'].observed, '3 XUSB host faults: ' + self.FIRST.format('3 errors'))


SUMMARY = 'SwitchUSB: DMA probe summary ({}): '
HEADER = (
    '{} held mappings that must be completed per transfer; maps/unmaps/held: own address 11/7/4, window 14/7/7, '
    'bounced 0/0/0; {} maps untracked (table full), {} unmaps of no tracked mapping; 0 MC errors, the latest '
    'INTSTATUS 0 ERR_STATUS 0 ERR_ADR 0'
)
CLASS = '{}: {} maps, {} unmaps, {} held; cache modes {} (bit n: mode n)'
LIVE = 'live: IOUSBHostDevice {} IOUSBHostInterface {} IOUSBHostPipe 1 AppleUSBHostDMABufferMemoryDescriptor 11'


def summary(second: float, when: str, **changes: int | str | tuple[int, int]) -> list:
    """Return one probe summary's lines (UsbDmaProbeHeld.cpp's formats): counts, two classes, live instances.

    `changes`: wrong (held mappings to complete), held (controller buffers), modes (the client buffers'), live.
    """
    wrong, held, modes, live = (
        changes.get(k, d) for k, d in (('wrong', 0), ('held', 11), ('modes', '0x10'), ('live', (1, 3)))
    )
    prefix = SUMMARY.format(when)
    lines = [
        HEADER.format(wrong, changes.get('untracked', 0), changes.get('unknown', 0)),
        CLASS.format(
            'AppleUSBHostDMABufferMemoryDescriptor', 20, 20 - held, held, changes.get('controller_modes', '0x10')
        ),
        CLASS.format('AppleUSBHostClientBufferMemoryDescriptor', 5, 5, 0, modes),
        LIVE.format(*live),
    ]
    return [record(second, prefix + line) for line in lines]


class Probe(unittest.TestCase):
    def test_a_clean_probe_passes_and_reports_the_window_and_the_live_objects(self):
        records = [
            *summary(1, 'host session start', live=(0, 0)),
            *summary(9, 'device role'),
            *summary(9.1, 'controller off'),
            *summary(12, 'host session start', live=(0, 0)),
            *summary(50, 'device role'),
            *summary(50.1, 'controller off'),
        ]
        _, rows = judge(records)
        self.assertEqual(rows['usb.dma.probe-summary'].verdict, PASS)
        self.assertIn('controller buffers 11 held at each of 2 controller offs', rows['usb.dma.probe-summary'].observed)
        self.assertEqual(
            rows['usb.watch.live-objects'].observed,
            'controller off 10:00:09.100: IOUSBHostDevice 1, IOUSBHostInterface 3; next session start 10:00:12.000: '
            'IOUSBHostDevice 0, IOUSBHostInterface 0 | controller off 10:00:50.100: IOUSBHostDevice 1, '
            'IOUSBHostInterface 3; no later session start',
        )

    def test_cached_client_payloads_retire_at_teardown(self):
        _, rows = judge(
            [*summary(3, 'lines used up', wrong=2, modes='0x8'), *summary(9, 'controller off', modes='0x8')]
        )
        self.assertEqual(rows['usb.dma.probe-summary'].verdict, PASS)
        self.assertIn('ClientBufferMemoryDescriptor cache modes 0x8', rows['usb.dma.probe-summary'].observed)

    def test_incomplete_observations_cannot_pass(self):
        for records in (
            summary(1, 'host session start'),
            summary(2, 'lines used up', wrong=1),
            summary(3, 'controller off', untracked=1),
            summary(4, 'device role', unknown=1),
        ):
            _, rows = judge(records)
            self.assertEqual(rows['usb.dma.probe-summary'].verdict, NO_DATA)

    def test_mappings_to_complete_a_buffer_not_write_combined_or_a_leak_fail(self):
        cases = {
            '2 held mappings to complete per transfer at device role 10:00:09.000': [
                *summary(9, 'device role', wrong=2),
                *summary(9.1, 'controller off'),
            ],
            'AppleUSBHostDMABufferMemoryDescriptor in cache modes 0x1, expected mode 4': [
                *summary(9.1, 'controller off', controller_modes='0x1'),
            ],
            'AppleUSBHostDMABufferMemoryDescriptor held at each controller off: 11 > 12': [
                *summary(9.1, 'controller off'),
                *summary(50.1, 'controller off', held=12),
            ],
        }
        for problem, records in cases.items():
            _, rows = judge(records)
            self.assertEqual(rows['usb.dma.probe-summary'].verdict, FAIL, problem)
            self.assertIn(problem, rows['usb.dma.probe-summary'].observed)

    def test_an_older_summary_format_is_not_read_as_a_pass(self):
        older = 'SwitchUSB: DMA probe summary (host role left): 12 map and 11 unmap calls; 1 still mapped'
        _, rows = judge([record(1, older)])
        self.assertEqual(rows['usb.dma.probe-summary'].verdict, INFO)
        self.assertEqual(
            rows['usb.dma.probe-summary'].observed, f'summary format not recognised; the last as printed: {older}'
        )


if __name__ == '__main__':
    unittest.main()
