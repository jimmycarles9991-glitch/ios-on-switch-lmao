"""The usb group over the emulator's runs of the port-contract branch and three device boots that never enumerated."""

import json
import re
import tempfile
import unittest
from pathlib import Path

from tools.devicerun import devicerun, usb
from tools.devicerun.rules import FAIL, INFO, NO_DATA, PASS, Event, Result

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = Path(__file__).resolve().parent / 'test' / 'usb'
CHAIN_PASSES = [
    'pd',
    'bringup',
    'role',
    'ioport',
    'transport',
    'port-power',
    'controller',
    'transfer-data',
    'enumeration',
    'configuration',
]
SOURCES = [
    ROOT / 'drivers/SwitchUSB' / name for name in ('SwitchUSBPort.cpp', 'SwitchXHCI.cpp', 'SwitchXHCIEvents.cpp')
]
LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
PREFIX = re.compile(r'^(?:Switch\w+|DK): ')


def judge(records: list[dict]) -> tuple[dict[str, Result], dict[str, Result]]:
    """Return the group's results and its summary rows, by name."""
    results, _ = devicerun.run_checks(records, usb.USB)
    by_check = {r.check.name: r for r in results}
    end = max((Event.from_ndjson(r).time for r in records), default=None)
    return by_check, {r.check.name: r for r in usb.rows(by_check, None, end)}


def fixture(name: str) -> tuple[dict[str, Result], dict[str, Result]]:
    return judge(list(devicerun.read_ndjson(str(FIXTURES / f'{name}.ndjson'))))


def layers(rows: dict[str, Result]) -> dict[str, str]:
    return {name.split('-', 1)[1]: r.verdict for name, r in rows.items() if name.startswith('usb.layer.')}


def failing(results: dict[str, Result]) -> set[str]:
    return {name for name, r in results.items() if r.verdict == FAIL}


def record(second: float, message: str, level: str = 'Default') -> dict:
    minute, second = divmod(second, 60)
    return {
        'eventType': 'logEvent',
        'timestamp': f'2026-11-14 10:{int(minute):02d}:{second:09.6f}+0000',
        'bootUUID': 'B1',
        'processImagePath': '/kernel.development',
        'messageType': level,
        'eventMessage': message,
    }


def joined_literals(path: Path) -> list[str]:
    """Return a C or C++ file's string literals, adjacent ones joined as the compiler joins them."""
    text = path.read_text(errors='replace')
    joined, current, last = [], None, 0
    for m in LITERAL.finditer(text):
        if current is not None and not text[last : m.start()].strip():
            current += m[1]
        else:
            if current is not None:
                joined.append(current)
            current = m[1]
        last = m.end()
    if current is not None:
        joined.append(current)
    return [s.replace('\\"', '"').replace('\\n', '\n') for s in joined]


def c_format(fragment: str, *args: object) -> str:
    """Return the line the driver logs: its format string (the one holding `fragment`) applied to `args`."""
    formats = [s for path in SOURCES for s in joined_literals(path) if fragment in s]
    if len(set(formats)) != 1:
        raise AssertionError(f'{fragment!r}: {len(set(formats))} format strings')
    return re.sub(r'%l?u|%d', '%d', formats[0].rstrip('\n')) % args


class Emulator(unittest.TestCase):
    """The emulator's runs of the device-build candidate (605d04ab4), with the driver's development lines."""

    def test_both_versions_pass_every_host_layer(self):
        for device in ('keyboard', 'storage', 'hub'):
            for ios in (27, 17):
                name = f'emu_g6_{device}_ios{ios}'
                results, rows = fixture(name)
                self.assertEqual([layers(rows)[n] for n in CHAIN_PASSES], [PASS] * len(CHAIN_PASSES), name)
                self.assertEqual(failing(results), set(), name)
                self.assertEqual(rows['usb.ctrl.reached-on'].verdict, PASS, name)
                self.assertEqual(rows['usb.ss.reset-order'].verdict, PASS, name)

    def test_a_role_cycle_keeps_its_transports_until_the_device_goes(self):
        for name in ('emu_cycle6_ios27', 'emu_cycle6_ios17'):
            results, rows = fixture(name)
            self.assertEqual(failing(results), set(), name)
            self.assertEqual(rows['usb.port.detach-order'].verdict, PASS, name)
            self.assertIn('kept until the device went', rows['usb.port.detach-order'].observed)

    def test_a_deferred_superspeed_reset_is_redone(self):
        for name in ('emu_g6_keyboard_ios27', 'emu_g6_hub_ios17'):
            _, rows = fixture(name)
            self.assertEqual(rows['usb.ss.reset-order'].verdict, PASS, name)
            self.assertIn('1 deferred, 1 taken', rows['usb.ss.reset-order'].observed, name)

    def test_each_class_driver_takes_its_device(self):
        results, rows = fixture('emu_g6_storage_ios17')
        self.assertEqual(layers(rows)['class'], PASS)
        self.assertIn('disk1', results['usb.class.storage'].observed)
        _, rows = fixture('emu_g6_keyboard_ios17')
        self.assertEqual(rows['usb.class.driver'].verdict, PASS)
        self.assertIn('kernel HID driver opened it +0.015 s', rows['usb.class.driver'].observed)
        results, rows = fixture('emu_g6_keyboard_ios27')
        self.assertEqual(results['usb.class.hid-deferred'].verdict, INFO)
        self.assertEqual(rows['usb.class.driver'].verdict, NO_DATA)
        for name in ('emu_g6_hub_ios27', 'emu_g6_hub_ios17'):
            results, _ = fixture(name)
            self.assertEqual(results['usb.class.hub'].verdict, PASS, name)
            self.assertEqual(results['usb.class.hub-port-disabled'].verdict, PASS, name)
            self.assertEqual(results['usb.iokit.bad-busy-count'].verdict, PASS, name)

    def test_a_boot_with_nothing_plugged_fails_nothing(self):
        for name in ('emu_g6_gpu_ios27', 'emu_g6_gpu_ios17'):
            results, rows = fixture(name)
            self.assertEqual(failing(results), set(), name)
            self.assertEqual(rows['usb.host.first-failure'].verdict, NO_DATA, name)
            self.assertIn('Port-USB-C@1/2/1', results['usb.port.found'].observed, name)

    def test_the_older_runs_fail_where_their_faults_were(self):
        results, rows = fixture('emu_cycle3_ios17')
        self.assertEqual(results['usb.port.transport-terminated'].verdict, FAIL)
        self.assertEqual(
            rows['usb.host.first-failure'].observed, 'transport; later failures: superspeed; no data before it: ioport'
        )
        _, rows = fixture('emu_cycle3_ios27')
        self.assertEqual(rows['usb.port.detach-order'].verdict, FAIL)
        self.assertIn('with AppleUSB20XHCIPort', rows['usb.port.detach-order'].observed)
        results, _ = fixture('emu_hub3_ios27')
        self.assertEqual(failing(results), {'usb.class.hub-port-disabled'})
        self.assertIn('QEMU USB HARDDRIVE', results['usb.class.hub-port-disabled'].observed)
        results, _ = fixture('emu_hub3_ios17')
        self.assertTrue({'usb.class.hub-port-disabled', 'usb.iokit.bad-busy-count'} <= failing(results))

    def test_values_only_a_device_gives(self):
        results, rows = fixture('emu_cycle6_ios27')
        self.assertIn('0x100/0x9000124/0xf8/0x180f009/1/1/15/0xc00/0x800/1', results['usb.info.capabilities'].observed)
        self.assertEqual(rows['usb.pad.bias-hs-disconnect'].verdict, PASS)
        self.assertEqual(rows['usb.pad.bias-10-8'].verdict, PASS)
        self.assertEqual(rows['usb.pad.port-cap-device'].verdict, PASS)
        self.assertIn('OTG_PAD0 5:0 = 6', rows['usb.pad.otg-pad0'].observed)
        self.assertIn('second host session', rows['usb.info.uncreated-ports'].observed)
        self.assertIn('2:0x80', rows['usb.info.uncreated-ports'].observed)
        self.assertIn('0/0', results['usb.info.pd-as-found'].observed)


ENUMERATED = 'AppleUSBHostPort::enumerateDeviceComplete_block_invoke'
COMPOSITE = 'AppleUSBHostCompositeDevice'
DESTROYING = 'AppleUSBHostPort::terminateDevice: destroying'
SERVICE = 'AppleUserUSBHostHIDDevice-0x1000004a3'
# The request is the device boot's (2026-11-14 19:17); its launch, start and open never came there, so those are
# IOUserServer.cpp's formats (DKS is "%s-0x%qx") with a guessed class name for the opened device.
DEXT = [
    f'DK: {SERVICE} waiting for server com.apple.driverkit.AppleUserHIDDrivers-1000004a3',
    f'DK: {SERVICE} server launched, validating',
    f'DK: {SERVICE}::start(IOUSBHostInterface-0x1000004a2) ok',
    'AppleUserUSBHostHIDDevice:0x1000004a3 open by AppleUserHIDEventDriver:0x1000004b1 (0x0)',
]
KERNEL_HID = [
    'IOUSBHostHIDDevice:0x1000002c1 start',
    'IOUSBHostHIDDevice:0x1000002c1 open by IOHIDEventDriver:0x1000002c5 (0x0)',
]


def session(*, ios: int = 27, power: tuple[str, ...] = ('0 -> 3',), detach: str = 'kept') -> list[dict]:
    """Return a host session in the driver's development lines, built from their format strings."""
    cls = (
        ('AppleUSB20XHCIPort', 'AppleUSB30XHCIPort')
        if ios == 27
        else ('AppleUSB20XHCITypeCPort', 'AppleUSB30XHCITypeCPort')
    )
    given = 'host port type 5, USB-C' if ios == 27 else 'Type-C port class'
    transport = 'SwitchUSB: USB-C port transport %s'
    lines = [
        (0.0, c_format('USB-C port %s@%s found', 'IOPort', '', 2, 1)),
        (1.0, c_format('root port %u created', 5, cls[0], 'USB 2.0', given)),
        (1.0, c_format('root port %u created', 1, cls[1], 'SuperSpeed', given)),
        (1.1, 'SwitchUSB: host controller up (AppleUSBXHCI, 1 interrupter, 3 shown to AppleUSBXHCI)'),
        (1.0, 'SwitchPD: classifying: the partner came up as our sink; source switches off 1 s'),
        (1.1, 'SwitchPD: sink-only partner: sourcing VBUS (v_vdd5v3, VCONN, OCP up; boost on)'),
        (1.2, 'SwitchUSB: host role (a device on the port, VBUS on)'),
        (1.3, c_format(transport, 'added', 2, 0, 'host', 'attach')),
        (1.4, f'{cls[0]}@00500000: AppleUSBHostPort::cableChangeOccurred: powering on'),
        (1.5, c_format('SuperSpeed reset requested', 1, 0x2A0)),
        (1.6, c_format('SuperSpeed reset on port %u deferred', 1, 0x2A0, 0x2A0)),
        (1.7, c_format('SuperSpeed reset requested', 1, 0x2A0)),
        (2.3, c_format('SuperSpeed reset taken', 1, 0x2A0, 0x80, 'taken')),
        (2.3, c_format(transport, 'added', 3, 1, 'host', 'SuperSpeed reset taken')),
        (2.5, f'{cls[0]}@00500000: {ENUMERATED}: enumerated 0x0627/0001/0000 (Keyboard / 1) at 12 Mbps'),
        (2.6, f'Keyboard@00500000: IOUSBHostDevice::setConfigurationGated: {COMPOSITE} selected configuration 1'),
        *[(2.61 + 0.01 * i, line) for i, line in enumerate(DEXT if ios == 27 else KERNEL_HID)],
    ]
    lines += [
        (1.3 + 0.1 * i, c_format('controller power state %lu', *map(int, step.split(' -> '))))
        for i, step in enumerate(power)
    ]
    gone = (8.1, f'{cls[0]}@00500000: {DESTROYING} 0x0627/0001/0000 (Keyboard): hardware connection lost')
    if detach == 'kept':
        lines += [
            (8.0, c_format(transport, 'kept until its device is torn down', n, n - 2, 'host', 'detach')) for n in (3, 2)
        ]
        lines += [
            gone,
            (8.2, c_format(transport, 'removed', 3, 1, 'host', 'detach')),
            (8.2, c_format(transport, 'removed', 2, 0, 'host', 'detach')),
        ]
    else:
        lines += [
            (8.0, c_format(transport, 'removed', 3, 1, 'host', 'detach')),
            (8.0, c_format(transport, 'removed', 2, 0, 'host', 'detach')),
            gone,
        ]
    lines += [(8.3, 'SwitchUSB: device role'), (30.0, 'tail')]
    return [record(second, message) for second, message in sorted(lines, key=lambda line: line[0])]


class OwnLines(unittest.TestCase):
    """Failures no run shows, in the driver's development lines built from their format strings."""

    def test_a_controller_left_at_power_state_0(self):
        records = [r for r in session() if 'controller power state' not in r['eventMessage']]
        records.insert(0, record(0.5, c_format('controller power state %lu', 1, 0)))
        _, rows = judge(records)
        self.assertEqual(rows['usb.ctrl.reached-on'].verdict, FAIL)
        self.assertTrue(
            rows['usb.ctrl.reached-on'].observed.startswith('a host transport was added and the controller never left')
        )
        self.assertEqual(rows['usb.host.first-failure'].observed.split(';')[0], 'controller')
        _, rows = judge(session(power=('0 -> 1',)))
        self.assertIn('did not reach On within 10 s', rows['usb.ctrl.reached-on'].observed)

    def test_a_host_transport_removed_under_its_device(self):
        _, rows = judge(session(detach='removed'))
        self.assertEqual(rows['usb.port.detach-order'].verdict, FAIL)
        self.assertIn('USB 2 (detach) with AppleUSB20XHCIPort', rows['usb.port.detach-order'].observed)
        self.assertEqual(rows['usb.host.first-failure'].observed, 'transport')

    def test_the_usb2_transport_stays_from_attach_to_detach(self):
        results, rows = judge(session())
        self.assertEqual(results['usb.port.usb2-removed'].verdict, PASS)
        for why in ('SuperSpeed reset taken', 'SuperSpeed reset owed', 'lane up', 'lane down'):
            records = [*session(), record(5.0, c_format('USB-C port transport %s', 'removed', 2, 0, 'host', why))]
            results, rows = judge(records)
            self.assertEqual(results['usb.port.usb2-removed'].verdict, FAIL, why)
            self.assertEqual(rows['usb.host.first-failure'].observed.split(';')[0], 'transport', why)
        swap = [*session(), record(5.0, c_format('USB-C port transport %s', 'removed', 2, 0, 'host', 'role change'))]
        self.assertEqual(judge(swap)[0]['usb.port.usb2-removed'].verdict, PASS)

    def test_a_mailbox_failure_is_in_its_commands_layer(self):
        enabled = c_format('mailbox busy for command', 1, 2)
        sspi = c_format('NAK %#x to mailbox command', 0, 16)
        stray = c_format('with no command waiting', 'ACK', 0)
        results, rows = judge([*session(), record(1.15, enabled)])
        self.assertEqual(results['usb.host.mailbox'].verdict, FAIL)
        self.assertEqual(results['usb.ss.mailbox'].verdict, PASS)
        self.assertEqual(rows['usb.host.first-failure'].observed.split(';')[0], 'bringup')
        results, rows = judge([*session(), record(2.2, sspi)])
        self.assertEqual(results['usb.host.mailbox'].verdict, PASS)
        self.assertEqual(results['usb.ss.mailbox'].verdict, FAIL)
        self.assertEqual(rows['usb.layer.12-superspeed'].verdict, FAIL)
        self.assertEqual(rows['usb.host.first-failure'].verdict, PASS)
        results, _ = judge([record(1, stray)])
        self.assertEqual(results['usb.host.mailbox'].verdict, FAIL)

    def test_device_mode_anywhere_and_back_on_the_mac(self):
        connected = 'SwitchUSB: host connected (high speed)'
        pull_up = 'SwitchUSB: functions registered, pull-up on (waiting for a host)'
        results, _ = judge([record(0.5, pull_up), record(0.6, connected), *session()])
        self.assertEqual(results['usb.dev.connected'].verdict, PASS)
        self.assertEqual(results['usb.dev.back-on-mac'].verdict, NO_DATA)
        results, _ = judge([*session(), record(9.0, pull_up), record(12.0, connected)])
        self.assertEqual(results['usb.dev.back-on-mac'].verdict, INFO)

    def test_the_superspeed_reset_order(self):
        def changed(old: str, new: str) -> list[dict]:
            return [{**r, 'eventMessage': r['eventMessage'].replace(old, new)} for r in session()]

        _, rows = judge(changed('0x2a0 -> 0x80, RESET_SSPI taken', '0x2a0 -> 0x2a0, RESET_SSPI taken'))
        self.assertIn('taken with PP still set (PORTSC 0x2a0)', rows['usb.ss.reset-order'].observed)
        results, rows = judge(changed('RESET_SSPI taken)', 'RESET_SSPI refused)'))
        self.assertEqual(results['usb.ss.refused'].verdict, FAIL)
        self.assertIn('USB 3 transport added before RESET_SSPI was taken', rows['usb.ss.reset-order'].observed)
        records = [
            r
            for r in session()
            if 'reset requested' not in r['eventMessage'] or r['timestamp'] < '2026-11-14 10:00:01.7'
        ]
        records = [r for r in records if 'reset taken' not in r['eventMessage']]
        _, rows = judge(records)
        self.assertIn('deferred and never requested again', rows['usb.ss.reset-order'].observed)

    def test_the_port_identity_and_the_device_role(self):
        records = [
            *session(),
            record(31, c_format('USB-C port %s@%s found', 'IOPort', '', 12, 1)),
            record(32, c_format('root port %u created', 6, 'AppleUSB20XHCIPort', 'USB 2.0', 'host port type 5')),
            record(40, c_format('USB-C port transport %s', 'added', 2, 1, 'device', 'role change')),
            record(40.5, 'AppleUSB20XHCIPort@00500000: AppleUSBHostPort::cableChangeOccurred: powering on'),
        ]
        results, _ = judge(records)
        for name in ('usb.port.found-wrong', 'usb.port.created-other', 'usb.port.device-role-powered'):
            self.assertEqual(results[name].verdict, FAIL, name)


class Lines(unittest.TestCase):
    def test_host_mode_off_fails_with_its_reason(self):
        results, rows = judge(
            [
                record(0, 'SwitchPD: classifying: the partner came up as our sink; source switches off 1 s'),
                record(1, 'SwitchPD: sink-only partner: sourcing VBUS (v_vdd5v3, VCONN, OCP up; boost on)'),
                record(2, 'SwitchUSB: host mode off: PLLRE did not lock (PLLRE_BASE 0x4 MISC0 0x0)'),
            ]
        )
        self.assertIn('PLLRE did not lock', results['usb.host.mode-off'].observed)
        self.assertEqual(rows['usb.host.first-failure'].observed, 'bringup')
        self.assertEqual(results['usb.host.role'].verdict, NO_DATA)

    def test_exceptions_fail_by_name_unless_a_wake_explains_them(self):
        exception = 'SwitchXHCI@00000000: AppleUSBHostController::hardwareExceptionThreadCallGated: '
        lost, saved = exception + '0x00000100 (lost power)', exception + '0x00000004 (save state failed)'
        recovery = exception + 'attempting recovery'
        wake = 'SwitchPower: 812.004 SwitchSuspend: wake 3: power key (PMC WAKE_STATUS 0x10 WAKE2_STATUS 0), devices'
        results, rows = judge([record(10, wake), record(11, lost), record(11.1, saved), record(11.2, recovery)])
        self.assertEqual(results['usb.ctrl.exception'].verdict, PASS)
        self.assertEqual(
            rows['usb.sleep.recoveries'].observed,
            'wake 3 10:00:10: 0x00000100 lost power, 0x00000004 save state failed; 1 recoveries',
        )
        results, _ = judge([record(10, wake), record(40, lost)])
        self.assertEqual(results['usb.ctrl.exception'].verdict, FAIL)
        self.assertIn('0x00000100/lost power', results['usb.ctrl.exception'].observed)

    def test_a_timed_out_address_device_names_the_interrupter_fold(self):
        failed = 'AppleUSB20XHCIPort@00500000: AppleUSBHostPort::createDevice: failed to create device (0xe00002d6)'
        results, rows = judge([record(1, failed)])
        self.assertTrue(results['usb.enum.fold'].observed.startswith('Address Device or the first descriptor read'))
        self.assertIn('usb.enum.fold', rows['usb.layer.09-enumeration'].observed)
        results, _ = judge([record(1, failed.replace('0xe00002d6', '0xe00002c0'))])
        self.assertEqual(results['usb.enum.fold'].verdict, PASS)
        self.assertEqual(results['usb.enum.failed'].verdict, FAIL)

    def test_the_failure_lines_fail_on_sight(self):
        lines = {
            'usb.port.identification-blocked': 'device identification blocked by transport restrictions',
            'usb.port.transports-supported': '[Port-USB-C@1] Could not find transports-supported property!',
            'usb.enum.failed': 'AppleUSB20XHCIPort@00500000: AppleUSBHostPort::enumerateDevice: enumeration failed',
            'usb.host.dma': 'SwitchUSB: DMA bounce pool full (65536 bytes asked)',
            'usb.dev.dci-bits': 'SwitchUSB: DCI bits 0x4: EP_PAUSE reads 0, not 0x4, 100 us after the change',
            'usb.dev.residue': 'SwitchUSB: XUDC transfer event with a residue larger than its TRB on DCI 3: TRB 0x1',
            'usb.host.not-loaded': 'SwitchUSB: host mode off: com.apple.iokit.IOUSBHostFamily did not load (is it',
            'usb.pd.sys-reset': 'SwitchPD: BM92T36 SYS_RESET: internal fault (status1 0x4)',
            'usb.port.transport-duplicate': '[ERROR] [Port-USB-C@1] Multiple transports of this type are not allowed '
            'on this port! (transportType: 2 [USB2], index: 1)',
            'usb.dev.smmu-off': 'SwitchUSB: device DMA by physical address, high and broken buffers bounced '
            '(SMMU off: no table)',
        }
        results, _ = judge([record(i, line) for i, line in enumerate(lines.values())])
        self.assertTrue(set(lines) <= failing(results), set(lines) - failing(results))
        results, _ = judge(
            [record(1, 'SwitchPD: BM92T36 SYS_RESET: found plugged as DFP, in DisplayPort mode (status1 0)')]
        )
        self.assertEqual(results['usb.pd.sys-reset'].verdict, PASS)

    def test_stats_and_panics(self):
        stats = (
            'SwitchXUDC: stats 5000 ms: IN 30.12 MB/s (1500 requests, 64 KiB bounced, 3 serial), OUT 1.00 MB/s '
            '(500 requests, 0 KiB bounced, 0 serial); 900 interrupts/s, 950 events/s; CPU 3.52% of one core '
            '(filter, events, queueing); bounced packets: 0 breaks, 0 64K, 12 above 4G, 0 cache-line edges; bounce '
            'pages free 2048/2048'
        )
        _, rows = judge([record(1, stats)])
        self.assertEqual(rows['usb.dev.stats-summary'].verdict, INFO)
        self.assertIn('busiest 400 requests/s at CPU 3.52 %', rows['usb.dev.stats-summary'].observed)
        self.assertIn('12 above 4G', rows['usb.dev.stats-summary'].observed)
        with tempfile.TemporaryDirectory() as crashes:
            body = {'panicString': 'panic(cpu 2 caller 0x1): SwitchUSB: XUSB falcon firmware hang (mailbox SMI)'}
            (Path(crashes) / 'panic-full-2026-11-14.ips').write_text('{"bug_type":"210"}\n' + json.dumps(body))
            self.assertEqual(usb.panic_row(crashes).verdict, FAIL)
            self.assertIn('XUSB falcon firmware hang', usb.panic_row(crashes).observed)


class Sources(unittest.TestCase):
    def test_switch_lines_have_their_format_strings(self):
        """Each Switch literal is in a format string of its source files (the first %s filled by another literal)."""
        for check in usb.USB:
            paths = [
                ROOT / part for part in check.source.split(', ') if re.fullmatch(r'(?:drivers|kits|kernel)/\S+', part)
            ]
            if not paths:
                continue
            strings = [s for path in paths for s in joined_literals(path)]
            strings += [f.replace('%s', a, 1) for f in strings if '%s' in f for a in strings if '%' not in a]
            for literal in check.literals():
                short = PREFIX.sub('', literal)
                self.assertTrue(any(literal in s or short in s for s in strings), f'{check.name}: {literal!r}')


if __name__ == '__main__':
    unittest.main()
