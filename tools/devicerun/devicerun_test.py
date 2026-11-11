"""tools/devicerun over synthetic `log show --style ndjson` lines and crash reports."""

import contextlib
import io
import re
import unittest
from pathlib import Path

from tools.devicerun import checks, devicerun, rules
from tools.devicerun.rules import FAIL, INFO, NO_DATA, PASS, Check

TEST = Path(__file__).resolve().parent / 'test'
NDJSON = TEST / 'device.ndjson'


def run(*check_list: Check) -> dict[str, rules.Result]:
    results, _ = devicerun.run_checks(devicerun.read_ndjson(str(NDJSON)), list(check_list))
    return {r.check.name: r for r in results}


class Checks(unittest.TestCase):
    def test_every_check_is_well_formed(self):
        names = [c.name for group in checks.GROUPS.values() for c in group]
        self.assertEqual(len(names), len(set(names)))
        for group in checks.GROUPS.values():
            for c in group:
                self.assertTrue(c.rule in {rules.REPORT, rules.TIMELINE} or rules.RULE.match(c.rule), c.name)
                self.assertTrue(all(c.literals()), c.name)
                if c.regex:
                    re.compile(c.regex)
                self.assertTrue(not c.needs or c.needs in names, c.name)
                self.assertTrue(not c.against or c.against in names, c.name)

    def test_predicate_escapes_and_covers_every_check(self):
        check = Check('q', 'o', 'say "hi" \\ there', process='kernel', subsystem='a.b', level='error')
        self.assertEqual(
            devicerun.clause(check),
            '((eventMessage CONTAINS "say \\"hi\\" \\\\ there") AND process BEGINSWITH "kernel" '
            'AND subsystem CONTAINS "a.b" AND messageType == error)',
        )
        self.assertIn('BEGINSWITH "SwitchFrame"', devicerun.predicate([check]))


class Rules(unittest.TestCase):
    def test_counts_pass_and_fail(self):
        got = run(
            Check('lskdd', 'o', 'Successfully spawned lskdd[', process='launchd'),
            Check('absent', 'o', 'never logged'),
            Check('present', 'o', 'TegraNVJPG: ', 'count > 0', process='kernel'),
        )
        self.assertEqual(got['lskdd'].verdict, FAIL)
        self.assertEqual(got['absent'].verdict, PASS)
        self.assertEqual(got['present'].verdict, PASS)

    def test_needs_turns_absence_into_no_data(self):
        got = run(
            Check('lane', 'o', 'SwitchUSB: SuperSpeed lane up', 'count > 0'),
            Check('reset', 'o', 'RESET_SSPI not taken', needs='lane'),
        )
        self.assertEqual(got['lane'].verdict, FAIL)
        self.assertEqual(got['reset'].verdict, NO_DATA)

    def test_boots_and_rate(self):
        got = run(
            Check('mg', 'o', 'MobileGestalt cache dropped', 'boots <= 1', show=('boot',)),
            Check('maild', 'o', 'Successfully spawned', 'rate <= 1', subsystem='com.apple.email.maild'),
        )
        self.assertEqual(got['mg'].verdict, FAIL)
        self.assertIn('2 boots', got['mg'].observed)
        self.assertEqual(got['maild'].verdict, PASS)

    def test_field_metrics_and_where(self):
        got = run(
            Check('nvjpg', 'o', 'TegraNVJPG: ', 'sum(decodes) > 0', regex=r'decodes=(?P<decodes>\d+)'),
            Check('imports', 'o', 'tegra-gm20b: imports ', 'p99(p99) <= 4000', regex=checks.GROUPS['gpu'][0].regex),
            Check(
                'idle',
                'o',
                'SwitchDisplay stats: ',
                'max(irqs/ms*1000) < 5',
                regex=r'(?P<ms>\d+) ms: (?P<irqs>\d+) DC A interrupts, (?P<swaps>\d+) swaps',
                where='swaps == 0',
            ),
            Check('none', 'o', 'TegraNVJPG: ', 'sum(missing) > 0', regex=r'decodes=(?P<decodes>\d+)'),
        )
        self.assertEqual(got['nvjpg'].verdict, PASS)
        self.assertEqual(got['imports'].verdict, FAIL)
        self.assertIn('5200', got['imports'].observed)
        self.assertEqual(got['idle'].verdict, PASS)
        self.assertIn('0.30', got['idle'].observed)
        self.assertEqual(got['none'].verdict, NO_DATA)

    def test_against_and_grouping(self):
        by_name = {c.name: c for c in checks.GROUPS['gpu'] + checks.GROUPS['refusals']}
        got = run(by_name['gpu.surface-locks'], by_name['gpu.rainbow'], by_name['refusal.user'])
        self.assertEqual(got['gpu.rainbow'].verdict, INFO)
        self.assertIn('2 surfaces, 1 without a gpu.surface-locks line: 78', got['gpu.rainbow'].observed)
        self.assertIn('backboardd/raw/format', got['refusal.user'].observed)

    def test_kernel_refusal_line(self):
        refusal = next(c for c in checks.GROUPS['refusals'] if c.name == 'refusal.kernel')
        self.assertIn('kernel_task/SwitchVIC/pitch x1', run(refusal)['refusal.kernel'].observed)

    def test_compute_view_refusals(self):
        view = next(c for c in checks.GROUPS['refusals'] if c.name == 'refusal.compute-view')

        def record(message: str, subsystem: str) -> dict:
            return {
                'timestamp': '2026-11-09 15:51:01.210000+0000',
                'bootUUID': 'B00T',
                'processImagePath': '/usr/libexec/PosterBoard',
                'subsystem': subsystem,
                'category': 'raw',
                'messageType': 'Error',
                'eventMessage': message,
            }

        boot17 = [
            record('PosterBoard[261] compute.view: a=0 (0x0) b=0 (0x0)', checks.REFUSAL_SUBSYSTEM),
            record(
                'PosterBoard[261] cb.m2v: command buffer on m2v (compute): a kernel m2v binds a view of another '
                'format at a texture slot [first predicate compute.view]',
                checks.REFUSAL_SUBSYSTEM,
            ),
            record('TegraGM20BMetal: PosterBoard raw 5s why=compute.view:7/7', checks.GPU_METAL_SUBSYSTEM),
        ]
        results, _ = devicerun.run_checks(boot17, [view])
        self.assertEqual(results[0].verdict, FAIL)
        self.assertIn('PosterBoard', results[0].observed)
        results, _ = devicerun.run_checks(boot17[2:], [view])
        self.assertEqual(results[0].verdict, PASS)

    def test_wifi_join_boot16(self):
        events = devicerun.read_ndjson(str(TEST / 'wifi.ndjson'))
        results, _ = devicerun.run_checks(events, checks.GROUPS['wifi'])
        got = {r.check.name: r for r in results}
        self.assertEqual(got['wifi.joined'].verdict, PASS)
        self.assertEqual(got['wifi.join-completion-failed'].verdict, FAIL)
        self.assertIn('-3902', got['wifi.join-completion-failed'].observed)
        self.assertNotIn('-100', got['wifi.join-completion-failed'].observed)
        self.assertEqual(got['wifi.join-report-unset'].verdict, FAIL)
        self.assertEqual(got['wifi.event-not-posted'].verdict, PASS)
        self.assertEqual(got['wifi.dhcp-link-local'].verdict, FAIL)
        self.assertEqual(got['wifi.ipv6-solicit-failed'].verdict, FAIL)

    def test_wifi_boot18_never_transmits(self):
        got = self.wifi(list(devicerun.read_ndjson(str(TEST / 'wifi_boot18.ndjson'))))
        self.assertEqual(got['wifi.joined'].verdict, PASS)
        self.assertEqual(got['wifi.en0-up'].verdict, FAIL)
        self.assertEqual(got['wifi.en0-output'].verdict, FAIL)
        self.assertIn('max(batches) 0', got['wifi.en0-output'].observed)
        self.assertEqual(got['wifi.tx-frames'].verdict, FAIL)
        self.assertEqual(got['wifi.dhcp-bound'].verdict, FAIL)
        self.assertEqual(got['wifi.dhcp-link-local'].verdict, FAIL)
        self.assertEqual(got['wifi.connection-id-refused'].verdict, FAIL)

    def test_wifi_en0_sending_and_a_lease_pass(self):
        sent = {
            'SwitchWLAN: stats ': ('tx 0 (0 KB)', 'tx 41 (6 KB)'),
            'SwitchWLAN: data ': ('en0 0 starts 0 batches', 'en0 9 starts 9 batches'),
        }
        records = []
        for record in devicerun.read_ndjson(str(TEST / 'wifi_boot18.ndjson')):
            message = record['eventMessage']
            if any(gone in message for gone in ('LINKLOCAL', 'no server', 'connection ID', 'CONNECTION_ID')):
                continue
            for prefix, (was, now) in sent.items():
                if message.startswith(prefix):
                    message = message.replace(was, now)
            records.append({**record, 'eventMessage': message})
        records.append({**records[-1], 'processImagePath': '/usr/libexec/configd', 'eventMessage': 'DHCP en0: BOUND'})
        records.insert(0, {**records[0], 'eventMessage': 'SwitchWLAN: en0 up, its output thread started'})
        got = self.wifi(records)
        self.assertEqual(got['wifi.en0-up'].verdict, PASS)
        self.assertEqual(got['wifi.en0-output'].verdict, PASS)
        self.assertEqual(got['wifi.tx-frames'].verdict, PASS)
        self.assertEqual(got['wifi.dhcp-bound'].verdict, PASS)
        self.assertEqual(got['wifi.dhcp-link-local'].verdict, PASS)
        self.assertEqual(got['wifi.connection-id-refused'].verdict, PASS)

    @staticmethod
    def wifi(records: list[dict]) -> dict[str, rules.Result]:
        results, _ = devicerun.run_checks(records, checks.GROUPS['wifi'])
        return {r.check.name: r for r in results}

    def test_wifi_checks_wait_for_a_join(self):
        got = run(*checks.GROUPS['wifi'])
        self.assertEqual(got['wifi.joined'].verdict, NO_DATA)
        self.assertEqual(got['wifi.join-completion-failed'].verdict, NO_DATA)
        self.assertEqual(got['wifi.dhcp-link-local'].verdict, NO_DATA)
        self.assertEqual(got['wifi.en0-output'].verdict, NO_DATA)
        self.assertEqual(got['wifi.dhcp-bound'].verdict, NO_DATA)


class UsbHost(unittest.TestCase):
    """The host-mode checks over boot-1109-2145's lines (a keyboard plugged twice) and a boot that enumerates."""

    @staticmethod
    def usb(records: list[dict]) -> dict[str, rules.Result]:
        results, _ = devicerun.run_checks(records, checks.GROUPS['usb'])
        return {r.check.name: r for r in results}

    @staticmethod
    def kernel(second: int, message: str, boot: str = 'B0000000-0000-0000-0000-000000000001') -> dict:
        return {
            'timestamp': f'2026-11-10 09:00:{second:02d}.000000+0000',
            'bootUUID': boot,
            'processImagePath': '/kernel.development',
            'subsystem': '',
            'category': '',
            'messageType': 'Default',
            'eventMessage': message,
        }

    def test_boot_1109_2145(self):
        got = self.usb(list(devicerun.read_ndjson(str(TEST / 'usb_host_boot1109.ndjson'))))
        for name in (
            'usb.host-save-state',
            'usb.host-exception',
            'usb.host-port-properties',
            'usb.host-connect',
            'usb.host-device-arrives',
        ):
            self.assertEqual(got[name].verdict, FAIL, name)
        self.assertIn('save state failed', got['usb.host-exception'].observed)

    def test_a_keyboard_that_enumerates(self):
        got = self.usb(
            [
                self.kernel(0, 'SwitchUSB: host controller up (AppleUSBXHCI, 1 interrupter, 3 shown to AppleUSBXHCI)'),
                self.kernel(1, 'SwitchUSB: host role (a device on the port, VBUS on)'),
                self.kernel(
                    2,
                    'SwitchUSB: host role +1 s (change 1, 1/2): xHCI USBCMD 0x801 USBSTS 0x8, pad 0 = port 5 '
                    '(USB2 5+5): U0 low speed, SS port 0 = port 1 (USB3 1+4): RxDetect nothing',
                ),
                self.kernel(3, 'SwitchUSB: USB device Keyboard (05ac:024f) registered at location 0x5000000'),
            ]
        )
        for name in (
            'usb.host-save-state',
            'usb.host-exception',
            'usb.host-port-properties',
            'usb.host-connect',
            'usb.host-device-arrives',
        ):
            self.assertEqual(got[name].verdict, PASS, name)

    def test_the_restore_after_sc7_excuses_its_exception(self):
        exception = (
            'SwitchXHCI@00000000: AppleUSBHostController::hardwareExceptionThreadCallGated: 0x00000004 '
            '(save state failed)'
        )
        up = 'SwitchUSB: host controller up (AppleUSBXHCI, 1 interrupter, 3 shown to AppleUSBXHCI)'
        restore = "SwitchUSB: XUSB host restore after SC7: the controller's state went with the falcon, USBSTS.SRE"
        got = self.usb([self.kernel(0, up), self.kernel(1, restore), self.kernel(2, exception)])
        self.assertEqual(got['usb.host-exception'].verdict, PASS)
        got = self.usb([self.kernel(0, up), self.kernel(2, exception)])
        self.assertEqual(got['usb.host-exception'].verdict, FAIL)


class Sbim(unittest.TestCase):
    CHECK = next(c for c in checks.GROUPS['display'] if c.name == 'display.sbim')

    @staticmethod
    def line(code: str) -> dict:
        return {
            'timestamp': '2026-11-08 19:11:01.701000-0800',
            'bootUUID': '31AB01C1',
            'processImagePath': '/usr/libexec/backboardd',
            'subsystem': '',
            'category': '',
            'messageType': 'Error',
            'eventMessage': f'libEDR - EDRServerEnableSBIM: IORegistryEntrySetCFProperty() => {code}',
        }

    def verdict(self, *codes: str) -> str:
        results, _ = devicerun.run_checks([self.line(c) for c in codes], [self.CHECK])
        return results[0].verdict

    def test_boot_15s_bad_argument_fails(self):
        self.assertEqual(self.verdict('0xe00002c2', '0xe00002c2'), FAIL)

    def test_the_one_unsupported_answer_passes(self):
        self.assertEqual(self.verdict('0xe00002c7'), PASS)
        self.assertEqual(self.verdict(), PASS)


class Reports(unittest.TestCase):
    def test_frames_and_crashes(self):
        _, frame_lines = devicerun.run_checks(devicerun.read_ndjson(str(NDJSON)), [])
        frame_rows = {r.check.name: r for r in devicerun.frame_rows(frame_lines)}
        self.assertEqual(frame_rows['frames.interaction'].verdict, FAIL)
        self.assertIn('p50 1 p99 3 periods', frame_rows['frames.interaction'].observed)
        crash = {r.check.name: r for r in devicerun.crash_rows(str(TEST / 'crashes'))}
        self.assertEqual(crash['crash.crashes'].verdict, FAIL)
        self.assertIn('SpringBoard x1', crash['crash.crashes'].observed)
        self.assertEqual(crash['crash.panics'].verdict, PASS)
        self.assertIn('JetsamEvent x1', crash['crash.jetsam'].observed)
        self.assertEqual(crash['crash.locationd'].verdict, FAIL)
        self.assertEqual(crash['crash.spotlightknowledged'].verdict, PASS)
        self.assertEqual(crash['crash.stale-sync-waiter'].verdict, FAIL)
        self.assertEqual(crash['crash.stale-sync-waiter'].observed, '2, newest locationd-2026-11-09-034109.ips')
        self.assertEqual(devicerun.crash_rows(None)[0].verdict, NO_DATA)

    def test_boot_label_selects_checks_and_steps(self):
        names = {c.name for group in devicerun.selected('A').values() for c in group}
        self.assertIn('gpu.rf16-rounding', names)
        self.assertNotIn('gpu.tiled-cache-on', names)
        self.assertIn('gpu.tiled-cache-on', {c.name for group in devicerun.selected(None).values() for c in group})
        self.assertIn('rf16-rounding', devicerun.checklist('A'))
        self.assertNotIn('rf16-rounding', devicerun.checklist('B'))

    def test_whole_run_exit_code(self):
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(devicerun.main([str(NDJSON), '--ndjson']), 1)
        self.assertIn('launchd.lskdd', out.getvalue())


if __name__ == '__main__':
    unittest.main()
