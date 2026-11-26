"""The usb group over the device boots, and the stale-data and class-driver rows over synthetic lines."""

import unittest

from tools.devicerun import devicerun
from tools.devicerun.rules import FAIL, FALLBACK, INFO, NO_DATA, PASS
from tools.devicerun.usb_test import DEXT, FIXTURES, failing, fixture, judge, layers, record

IOS27 = 'Darwin Kernel Version 25.5.0: switch-ios:ios-obj/DEVELOPMENT_ARM64_BCM2837'
IOS17 = 'Darwin Kernel Version 23.6.0: switch-ios:ios-obj/DEVELOPMENT_ARM64_BCM2837'
PORT = 'AppleUSB20XHCIPort@00500000: AppleUSBHostPort::'
ENUMERATED = f'{PORT}enumerateDeviceComplete_block_invoke: enumerated 0x3434/0110/0110 (IOUSBHostDevice / 1) at 12 Mbps'
CONFIGURED = 'IOUSBHostDevice@00500000: IOUSBHostDevice::setConfigurationGated: AppleUSBHostCompositeDevice selected '
CONFIGURED += 'configuration 1'
GONE = f'{PORT}terminateDevice: destroying 0x3434/0110/0110 (IOUSBHostDevice): hardware connection lost'
SERVICE = DEXT[0].split()[1]
WAIT = DEXT[0]
KERNEL = [
    'IOUSBHostHIDDevice:0x1000002c1 start',
    'IOUSBHostHIDDevice:0x1000002c1 open by IOHIDEventDriver:0x1000002c5 (0x0)',
]
FRAGMENT = 'IOUSBHostDevice@00500000: IOUSBHostDevice::start_block_invoke: device descriptor fragment is invalid'
MAX_PACKET = (
    'IOUSBHostFamily::validateEndpointMaxPacketSize: USB 2.0 5.[5-8].3: endpoint 0x00 invalid wMaxPacketSize 0x0000'
)
FAILED_ENUMERATION = f'{PORT}enumerateDeviceComplete_block_invoke: enumeration failed'
DESCRIPTOR = (
    'IOUSBHostDevice@00500000: IOUSBHostDevice::getDescriptorGated: type 0x{} index 0 length 18: completed with '
)
DESCRIPTOR += '0x00000000 bytesTransferred 18 type 0x{}'


def failed(retries: int) -> str:
    server = 'com.apple.driverkit.AppleUserHIDDrivers-1000004a3'
    return f'DK: {SERVICE} failed to find server {server}, remaining 0ms, retries {retries}'


def plugged(*driver: tuple[float, str], kernel: str = IOS27, gone: float | None = 17.0, end: float = 500.0) -> list:
    """Return a keyboard configured at 1.01 s with these driver lines, torn down at `gone`, the log ending at `end`."""
    lines = [
        (0.0, kernel),
        (0.5, 'SwitchUSB: host role (a device on the port, VBUS on)'),
        (0.6, f'{PORT}cableChangeOccurred: powering on'),
        (1.0, ENUMERATED),
        (1.01, CONFIGURED),
        *driver,
        (end, 'tail'),
    ]
    if gone is not None:
        lines += [(gone - 0.1, 'SwitchUSB: device role'), (gone, GONE)]
    return [record(second, message) for second, message in sorted(lines, key=lambda line: line[0])]


def driverkitd(second: float) -> dict:
    return {**record(second, 'Received MIG message', 'Info'), 'processImagePath': '/usr/libexec/driverkitd'}


class Device(unittest.TestCase):
    def test_superspeed_requires_actual_enumeration(self):
        for speed in ('12 Mbps', '480 Mbps', '5000 Mbps'):
            with self.subTest(speed=speed):
                log = [
                    record(0, 'SwitchUSB: SuperSpeed lane up (pairs free, host role, UPHY PLL calibrated)'),
                    record(1, ENUMERATED.replace('12 Mbps', speed)),
                ]
                results, _ = judge(log)
                expected = INFO if speed == '5000 Mbps' else NO_DATA
                self.assertEqual(results['usb.ss.enumerated'].verdict, expected)

    def test_superspeed_device_requires_the_negotiated_speed(self):
        for speed in ('full', 'high', 'super'):
            with self.subTest(speed=speed):
                results, _ = judge([record(0, f'SwitchUSB: host connected ({speed} speed)')])
                self.assertEqual(results['usb.dev.superspeed'].verdict, INFO if speed == 'super' else NO_DATA)

    def test_boot_1109_fails_the_port_identity_then_the_save_state(self):
        results, rows = fixture('device_1109_2145')
        self.assertEqual(layers(rows)['ioport'], FAIL)
        self.assertEqual(layers(rows)['controller'], FAIL)
        self.assertIn('usb.ctrl.save-state', rows['usb.layer.07-controller'].observed)
        self.assertIn('save state failed', results['usb.ctrl.exception'].observed)
        self.assertEqual(
            rows['usb.host.first-failure'].observed, 'ioport; later failures: controller, enumeration, superspeed'
        )
        self.assertEqual([layers(rows)[n] for n in ('pd', 'bringup', 'role', 'port-power')], [PASS] * 4)

    def test_boot_1112_stops_at_the_controller_left_at_power_state_0(self):
        results, rows = fixture('device_1112_1500')
        self.assertEqual([layers(rows)[n] for n in ('pd', 'bringup', 'role', 'port-power')], [PASS] * 4)
        self.assertEqual(layers(rows)['ioport'], NO_DATA)
        self.assertEqual(layers(rows)['controller'], FAIL)
        self.assertIn('deadline passed (PORTSC 0x00000000)', rows['usb.layer.07-controller'].observed)
        self.assertIn('0x800/0x19 x2', results['usb.ctrl.halted'].observed)
        self.assertEqual(
            rows['usb.host.first-failure'].observed,
            'controller; later failures: enumeration, superspeed, device; no data before it: ioport, transport',
        )
        self.assertEqual(results['usb.ctrl.save-state'].verdict, PASS)
        self.assertIn(
            '15:01:33.839 host connected (high speed), 0.001 s after the PD detach at 15:01:33.838',
            rows['usb.dev.no-partner'].observed,
        )

    def test_boot_1114_fails_at_the_transport_then_the_superspeed_mailbox(self):
        results, rows = fixture('device_1114_1245')
        self.assertEqual(rows['usb.host.first-failure'].observed, 'transport; later failures: enumeration, superspeed')
        self.assertEqual(
            [layers(rows)[n] for n in ('pd', 'bringup', 'role', 'ioport', 'port-power', 'controller', 'device')],
            [PASS] * 7,
        )
        self.assertIn('USB2 x4', results['usb.port.transport-duplicate'].observed)
        self.assertIn('host/SuperSpeed reset taken x4', results['usb.port.usb2-removed'].observed)
        self.assertEqual(results['usb.host.mailbox'].verdict, PASS)
        self.assertIn('falcon mailbox command 16 not taken', results['usb.ss.mailbox'].observed)
        self.assertIn('usb.ss.mailbox', rows['usb.layer.12-superspeed'].observed)
        self.assertEqual(results['usb.port.usb2'].verdict, NO_DATA, "Apple's port-properties line: none on a device")
        self.assertEqual(results['usb.dev.connected'].verdict, PASS, 'connected at 12:45:57, before the host sessions')
        self.assertEqual(results['usb.dev.back-on-mac'].verdict, NO_DATA, 'step 4 not done')

    def test_boot_1114_1415_reads_stale_descriptors_and_its_keyboard_gets_no_driver(self):
        results, rows = fixture('device_1114_1415')
        self.assertEqual(
            rows['usb.host.first-failure'].observed, 'transfer-data; later failures: enumeration, class, device'
        )
        passing = ('pd', 'bringup', 'role', 'ioport', 'transport', 'port-power', 'controller', 'configuration')
        self.assertEqual([layers(rows)[n] for n in (*passing, 'superspeed')], [PASS] * 9)
        self.assertEqual(
            rows['usb.dev.no-partner'].observed,
            'device-mode connect reported with no partner: 14:17:45.388 host connected (high speed), 0.005 s after '
            'the PD detach at 14:17:45.383 (detached: VBUS, v_vdd5v3, VCONN off), no attach between',
        )
        self.assertTrue(results['usb.dev.connected'].observed.startswith('2 lines'))
        self.assertTrue(results['usb.dev.back-on-mac'].observed.startswith('1 lines'))
        stale = rows['usb.enum.stale-data'].observed
        self.assertTrue(stale.startswith('6 of 6 enumeration attempts in 2 host sessions read stale or zero data'))
        self.assertIn('14:17:28 0x3434/0110/0110: 2 of 2 attempts stale', stale)
        self.assertIn('type 0x03 read as type 0x01 x2), attempt 2 enumerated it, reads in that attempt stale', stale)
        self.assertIn('14:18:07 an unidentified device at 00500000: 4 of 4 attempts stale', stale)
        self.assertIn('type 0x01 read as type 0x00 x2), none enumerated', stale)
        driver = rows['usb.class.driver']
        self.assertEqual(driver.verdict, FAIL)
        self.assertTrue(driver.observed.startswith('driver never started: 0x3434/0110/0110 (IOUSBHostDevice) config'))
        self.assertIn('none by its teardown at +15.644 s (hardware connection lost)', driver.observed)
        self.assertIn('2 driverkitd MIG messages in its first 5 s; attach to detach 17.3 s', driver.observed)
        self.assertIn('14:17:54 14:18:07', results['usb.enum.after-role'].observed)
        self.assertEqual(results['usb.port.transport-duplicate'].verdict, PASS)
        self.assertIn('4 requested, 0 deferred, 4 taken', rows['usb.ss.reset-order'].observed)
        self.assertIn('SuperSpeed root port power-off/1/done/700', results['usb.info.settle'].observed)

    def test_boot_1114_1917_the_first_passing_host_boot_is_its_keyboard_through_the_kernel_fallback(self):
        results, rows = fixture('device_1114_1917')
        self.assertEqual(rows['usb.host.first-failure'].observed, 'every layer passes; through a fallback: class')
        verdicts = layers(rows)
        self.assertEqual({n: v for n, v in verdicts.items() if v != PASS}, {'class': FALLBACK})
        reason = "the kernel refused 3 dext processes their dyld cache: not on process' root volume nor preboot volume"
        hid = 'com.apple.driverkit.AppleUserHIDDrivers'
        self.assertEqual(
            rows['usb.class.driver'].observed,
            f'0x1050/0407/0574 (YubiKey OTP+FIDO+CCID): dext requested +0.010 s, then 3 launches of {hid} died '
            f'({reason}), the last +4.229 s, then kernel HID driver started +4.229 s, then 3 launches of its event '
            f"driver's dext died ({reason}), the last +6.286 s, then IOHIDEventDriver opened it +6.287 s; "
            '0x1050/0407/0574 (YubiKey OTP+FIDO+CCID): on the kernel fallback at its teardown at +7.547 s (hardware '
            f'connection lost): dext requested +0.010 s, then 2 launches of {hid} died ({reason}), the last +7.046 s; '
            'the kernel HID driver had not started; attach to detach 8.1 s',
        )
        self.assertEqual(results['usb.dext.shared-region'].verdict, FAIL)
        self.assertIn(
            '9 lines; com.apple.AppleU/dyld_shared_cache_arm64e x9', results['usb.dext.shared-region'].observed
        )
        self.assertEqual(results['usb.ctrl.halted'].verdict, PASS, 'the 19:20:12.460 report predates its host role')
        self.assertEqual(results['usb.class.hid-kernel'].verdict, INFO, 'its FIDO interface has no event driver')
        self.assertEqual(
            rows['usb.enum.stale-data'].observed,
            '0 of 2 enumeration attempts in 2 host sessions read stale or zero data',
        )
        self.assertEqual(rows['usb.dev.no-partner'].verdict, PASS)
        self.assertTrue(rows['usb.dma.probe-summary'].observed.startswith('7 summaries: 0 held mappings to complete'))
        self.assertIn('controller buffers 11 held at each of 3 controller offs', rows['usb.dma.probe-summary'].observed)
        self.assertIn('a 16384-page window', rows['usb.dma.window'].observed)
        self.assertIn(
            'window 28 of 29; complete lines with the buffer above 4 GiB: own address 0 of 15, window 22 of 22',
            rows['usb.dma.window'].observed,
        )
        watch = {
            'usb.watch.ss-power-off': '1 in 2 host sessions: 19:20:00.527: 1 (done after 700 ms); 19:20:12.071: 0',
            'usb.watch.pd-mac': 'request aborted at attach x1 (19:21:06.599); no data role swap, SEND_VDM as UFP '
            'aborted x1 (19:21:07.165)',
            'usb.watch.transaction-errors': '4 at 2 teardowns (19:20:08.662: 0x83; 19:20:49.894: 0x83 0x81 0x84)',
            'usb.watch.live-objects': 'controller off 19:20:08.669: IOUSBHostDevice 1, IOUSBHostInterface 3; next '
            'session start 19:20:12.070: IOUSBHostDevice 0, IOUSBHostInterface 0 | controller off 19:20:49.905: '
            'IOUSBHostDevice 1, IOUSBHostInterface 3; no later session start',
        }
        self.assertEqual({name: rows[name].observed for name in watch}, watch)

    def test_a_pull_that_starts_after_the_first_session_judges_the_same(self):
        records = [
            r
            for r in devicerun.read_ndjson(str(FIXTURES / 'device_1112_1500.ndjson'))
            if r['timestamp'] > '2026-11-12 15:01'
        ]
        results, rows = judge(records)
        self.assertEqual(results['usb.host.up'].verdict, NO_DATA)
        self.assertEqual(layers(rows)['bringup'], PASS)
        self.assertEqual(rows['usb.host.first-failure'].observed.split(';')[0], 'controller')


class OneBoot(unittest.TestCase):
    PULL_UP = 'SwitchUSB: functions registered, pull-up on (waiting for a host)'
    CONNECTED = 'SwitchUSB: host connected (high speed)'
    STATS = (
        'SwitchXUDC: stats 2001 ms: IN 30.00 MB/s (3000 requests, 0 KiB bounced, 0 serial), OUT 0.05 MB/s (100 '
        'requests, 0 KiB bounced, 0 serial); 900 interrupts/s, 950 events/s; CPU 19.26% of one core (filter, events, '
        'queueing); bounced packets: 0 breaks, 0 64K, 0 above 4G, 0 cache-line edges; bounce pages free 2048/2048'
    )

    def test_the_usb_group_judges_the_last_boot_that_starts_in_the_archive(self):
        earlier = [{**record(second, line), 'bootUUID': 'A1000000'} for second, line in ((1, self.STATS), (2, 'x'))]
        later = [record(60, IOS27), record(61, self.PULL_UP), record(62, self.CONNECTED)]
        groups = dict(devicerun.evaluate_all(earlier + later, None, 'U'))
        usb_rows = {r.check.name: r for r in groups['usb-layers']}
        self.assertEqual(
            usb_rows['usb.boot'].observed,
            'boot B1 2026-11-14 10:01:00 to 10:01:02; 1 other boot in the archive: A1000000 2026-11-14 10:00:01 to '
            '10:00:02, no kernel line (it started before the archive)',
        )
        self.assertEqual(usb_rows['usb.dev.stats-summary'].verdict, NO_DATA)
        self.assertEqual({r.check.name: r for r in groups['usb']}['usb.dev.connected'].verdict, PASS)
        groups = dict(devicerun.evaluate_all(earlier + later, None, 'U', boot='a1'))
        usb_rows = {r.check.name: r for r in groups['usb-layers']}
        self.assertIn('busiest 1549 requests/s at CPU 19.26 %', usb_rows['usb.dev.stats-summary'].observed)
        self.assertTrue(usb_rows['usb.boot'].observed.startswith('boot A1000000 2026-11-14 10:00:01 to 10:00:02, no'))

    def test_a_connect_on_an_empty_port_is_its_own_failure(self):
        lines = [
            (1, self.PULL_UP),
            (2, 'SwitchPD: sink: the partner is a source (charger, host or dock) (alert 0 status1 0x4580 status2 0x1)'),
            (3, self.CONNECTED),
            (10, 'SwitchPD: detached (alert 0x8 status1 0 status2 0)'),
            (10.004, self.CONNECTED),
            (
                20,
                'SwitchPD: sink: the partner is a source (charger, host or dock) (alert 0 status1 0x4580 status2 0x1)',
            ),
            (20.5, self.CONNECTED),
        ]
        results, rows = judge([record(second, line) for second, line in lines])
        self.assertEqual(rows['usb.dev.no-partner'].verdict, FAIL)
        self.assertIn(
            '10:00:10.004 host connected (high speed), 0.004 s after the PD detach at 10:00:10.000 (detached)',
            rows['usb.dev.no-partner'].observed,
        )
        self.assertTrue(results['usb.dev.connected'].observed.startswith('2 lines'))
        self.assertIn('usb.dev.no-partner', rows['usb.layer.13-device'].observed)


class StaleData(unittest.TestCase):
    def test_a_retry_that_reads_correctly_still_fails_the_layer(self):
        first = [(0.8, FRAGMENT), (0.8, MAX_PACKET), (0.801, FAILED_ENUMERATION), (0.9, DESCRIPTOR.format('01', '01'))]
        results, rows = judge(plugged(*first, *[(1.02 + 0.2 * i, line) for i, line in enumerate(DEXT)]))
        stale = rows['usb.enum.stale-data']
        self.assertEqual(stale.verdict, FAIL)
        self.assertIn('1 of 2 enumeration attempts in 1 host sessions', stale.observed)
        self.assertIn('attempt 2 enumerated it and read correctly', stale.observed)
        self.assertTrue(
            rows['usb.host.first-failure'].observed.startswith('transfer-data; later failures: enumeration;')
        )
        self.assertEqual(len(results['usb.enum.stale-lines'].matches), 2, 'a read that got its own type is no fault')

    def test_zeros_on_every_attempt(self):
        attempts = [(1.0 + i, DESCRIPTOR.format('01', '00')) for i in range(3)]
        attempts += [(1.0005 + i, FAILED_ENUMERATION) for i in range(3)]
        records = [record(0.5, 'SwitchUSB: host role (a device on the port, VBUS on)')]
        records += [record(second, line) for second, line in sorted(attempts)]
        _, rows = judge(records)
        self.assertIn(
            'an unidentified device at 00500000: 3 of 3 attempts stale (type 0x01 read as type 0x00 x3), none',
            rows['usb.enum.stale-data'].observed,
        )

    def test_a_hub_session_counts_each_device_on_its_own_port(self):
        hub_port = 'AppleUSB20HubPort@005{}0000: AppleUSBHostPort::enumerateDeviceComplete_block_invoke: '
        lines = [
            (0.5, 'SwitchUSB: host role (a device on the port, VBUS on)'),
            (1.0, f'{PORT}enumerateDeviceComplete_block_invoke: enumerated 0x2109/2817/0100 (Hub / 1) at 480 Mbps'),
            (1.2, FRAGMENT.replace('@00500000', '@00510000')),
            (1.2, MAX_PACKET),
            (1.201, hub_port.format(1) + 'enumeration failed'),
            (1.3, DESCRIPTOR.format('01', '00').replace('@00500000', '@00520000')),
            (1.301, hub_port.format(2) + 'enumeration failed'),
            (2.0, hub_port.format(1) + 'enumerated 0x3434/0110/0110 (Keyboard / 1) at 12 Mbps'),
            (2.5, hub_port.format(2) + 'enumerated 0x0781/5581/0100 (Stick / 1) at 480 Mbps'),
        ]
        _, rows = judge([record(second, line) for second, line in lines])
        self.assertEqual(
            rows['usb.enum.stale-data'].observed,
            '2 of 5 enumeration attempts in 1 host sessions read stale or zero data (3 lines): 10:00:00 '
            '0x3434/0110/0110: 1 of 2 attempts stale (device descriptor fragment invalid, endpoint 0 wMaxPacketSize '
            '0x0000), attempt 2 enumerated it and read correctly; 10:00:00 0x0781/5581/0100: 1 of 2 attempts stale '
            '(type 0x01 read as type 0x00), attempt 2 enumerated it and read correctly',
        )


class Driver(unittest.TestCase):
    """A configured device's class driver, step by step, with times from its configuration."""

    def test_the_dext_requested_launched_started_and_opened(self):
        _, rows = judge(plugged(*[(1.02 + 0.2 * i, line) for i, line in enumerate(DEXT)]))
        self.assertEqual(rows['usb.class.driver'].verdict, PASS)
        self.assertEqual(
            rows['usb.class.driver'].observed,
            '0x3434/0110/0110 (IOUSBHostDevice): dext request +0.010 s, launch +0.210 s, start +0.410 s, '
            'HID device opened +0.610 s',
        )

    def test_the_kernel_hid_driver_on_17_and_the_fallback_on_27(self):
        _, rows = judge(plugged((1.03, KERNEL[0]), (1.04, KERNEL[1]), kernel=IOS17))
        self.assertEqual(rows['usb.class.driver'].verdict, PASS)
        self.assertIn('kernel HID driver opened it +0.030 s', rows['usb.class.driver'].observed)
        _, rows = judge(plugged((1.03, KERNEL[0]), kernel=IOS17))
        self.assertTrue(
            rows['usb.class.driver'].observed.endswith(
                'kernel HID driver started +0.020 s and nothing opened it within 5 s; attach to detach 16.4 s'
            )
        )
        died = [(1.02, WAIT), (121, failed(2)), (241, failed(1)), (250, KERNEL[0]), (250.5, KERNEL[1])]
        results, rows = judge(plugged(*died, gone=None))
        self.assertEqual(rows['usb.class.driver'].verdict, FALLBACK)
        self.assertEqual(
            rows['usb.class.driver'].observed,
            '0x3434/0110/0110 (IOUSBHostDevice): dext requested +0.010 s, then 2 launches of '
            'com.apple.driverkit.AppleUserHIDDrivers died (no reason in the log), the last +239.990 s, then kernel HID '
            'driver started +248.990 s, then IOHIDEventDriver opened it +249.490 s',
        )
        self.assertEqual(rows['usb.layer.11-class'].verdict, FALLBACK)
        self.assertEqual(failing(results), set())
        self.assertIn('through a fallback: class', rows['usb.host.first-failure'].observed)

    def test_no_driver_fails_at_the_first_step_missing_however_short_the_attach(self):
        _, rows = judge([*plugged(gone=3.0), driverkitd(1.02)])
        observed = rows['usb.class.driver'].observed
        self.assertEqual(rows['usb.class.driver'].verdict, FAIL)
        self.assertTrue(observed.startswith('driver never started: 0x3434/0110/0110 (IOUSBHostDevice) configured '))
        self.assertIn(
            'no dext request, dext start or kernel class driver line within 5 s of its configuration', observed
        )
        self.assertIn('none by its teardown at +1.990 s (hardware connection lost)', observed)
        self.assertIn('1 driverkitd MIG messages in its first 5 s; attach to detach 2.4 s', observed)
        _, rows = judge(plugged((1.02, WAIT)))
        self.assertIn(
            'no dext launch within 5 s of its dext request (+0.010 s), none by its teardown',
            rows['usb.class.driver'].observed,
        )
        _, rows = judge(plugged((1.02, WAIT), (31, DEXT[1]), gone=40))
        self.assertIn(
            'no dext launch within 5 s of its dext request (+0.010 s), first at +29.990 s',
            rows['usb.class.driver'].observed,
        )
        _, rows = judge(plugged(*[(1.02 + 0.2 * i, line) for i, line in enumerate(DEXT[:3])]))
        self.assertIn('no HID device opened within 5 s of its start (+0.410 s)', rows['usb.class.driver'].observed)
        _, rows = judge(plugged((1.02, DEXT[2].replace(') ok', ') fail'))))
        self.assertEqual(rows['usb.class.driver'].verdict, FAIL)
        self.assertTrue(
            rows['usb.class.driver'].observed.endswith('::start(IOUSBHostInterface-0x1000004a2) fail +0.010 s')
        )

    def test_a_hid_device_opened_without_driverkit_lines_passes_and_says_so(self):
        _, rows = judge(plugged((2.5, DEXT[3])))
        self.assertEqual(rows['usb.class.driver'].verdict, PASS)
        self.assertIn(
            'HID device AppleUserUSBHostHIDDevice opened +1.490 s, no DriverKit line', rows['usb.class.driver'].observed
        )

    def test_a_log_that_ends_inside_a_step_has_no_data(self):
        _, rows = judge(plugged(gone=None, end=2.0))
        self.assertEqual(rows['usb.class.driver'].verdict, NO_DATA)
        _, rows = judge(plugged((1.02, WAIT), gone=None, end=3.0))
        self.assertEqual(rows['usb.class.driver'].verdict, NO_DATA)


if __name__ == '__main__':
    unittest.main()
