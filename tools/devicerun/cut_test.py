"""cut.py's checks and reports.py's cut rows over lines and crash reports written as the cut's sources write them."""

import json
import tempfile
import unittest
from pathlib import Path

from tools.devicerun import devicerun, reports
from tools.devicerun.cut import CUT, GPU_METAL_SUBSYSTEM, REFUSAL_SUBSYSTEM
from tools.devicerun.rules import FAIL, INFO, NO_DATA, PASS

PATH_ON = 'TegraGM20BMetal: raw GM20B path on in {who} (pack raw.pack, stack 00ff00ff00ff00ff)'
READY = 'TegraGM20BMetal: device NVIDIA Tegra X1 (GM20B) ready in {who}, open 41 ms'
RATES = 'gm20b 5s c=310(62.0/s) enc=120us/c d=4.0 p=1.0 b=0.00 defer=0 rw=0 pool=48MB/3 grow=0'
RAISE = (
    "*** Terminating app due to uncaught exception 'NSInvalidArgumentException', reason: "
    "'-[SwitchMetalDevice hostImport:]: unrecognized selector sent to instance 0x10a2c4000'"
)


def record(process: str, message: str, subsystem: str = '', second: int = 0) -> dict:
    return {
        'timestamp': f'2026-11-19 09:12:{second:02d}.000000-0800',
        'bootUUID': '7C1E0A22',
        'processImagePath': '/System/Library/CoreServices/' + process,
        'subsystem': subsystem,
        'category': 'gm20b',
        'messageType': 'Default',
        'eventMessage': message,
    }


def results(*records: dict) -> dict:
    found, _ = devicerun.run_checks(list(records), CUT)
    return {r.check.name: r for r in found}


def clean_boot() -> list[dict]:
    return [
        record('SpringBoard', PATH_ON.format(who='SpringBoard'), GPU_METAL_SUBSYSTEM),
        record('SpringBoard', READY.format(who='SpringBoard'), GPU_METAL_SUBSYSTEM),
        record('backboardd', RATES, GPU_METAL_SUBSYSTEM, 5),
    ]


class Cut(unittest.TestCase):
    def test_a_boot_after_the_cut_passes(self):
        got = results(*clean_boot())
        for name in ('cut.m2v-opener', 'cut.no-device', 'cut.path-off', 'cut.rates-fields', 'cut.selector-raised'):
            self.assertEqual(got[name].verdict, PASS, name)
        self.assertEqual(got['cut.rates'].verdict, INFO)
        self.assertIn('0 without a cut.path-on line', got['cut.device-ready'].observed)

    def test_an_old_plugin_fails_by_its_lines(self):
        got = results(
            *clean_boot(),
            record(
                'SpringBoard', 'SwitchMetal: SpringBoard opened its m2v device for enc.planarrt', GPU_METAL_SUBSYSTEM, 1
            ),
            record('SpringBoard', 'TegraGM20BMetal: raw GM20B path off in SpringBoard', GPU_METAL_SUBSYSTEM, 2),
            record('backboardd', RATES + ' m2v=3 replay=1', GPU_METAL_SUBSYSTEM, 6),
        )
        self.assertEqual(got['cut.m2v-opener'].verdict, FAIL)
        self.assertEqual(got['cut.path-off'].verdict, FAIL)
        self.assertEqual(got['cut.rates-fields'].verdict, FAIL)
        self.assertIn('m2v', got['cut.rates-fields'].observed)

    def test_a_process_left_out_on_purpose_is_not_a_missing_device(self):
        left_out = 'TegraGM20BMetal: no device in mediaserverd (raw): left out (TEGRA_GPU_ALL_CLIENTS=0)'
        missing = 'TegraGM20BMetal: no device in PosterBoard (raw): no AGXAccelerator'
        self.assertEqual(results(record('mediaserverd', left_out))['cut.no-device'].verdict, PASS)
        got = results(record('PosterBoard', missing))['cut.no-device']
        self.assertEqual(got.verdict, FAIL)
        self.assertIn('PosterBoard', got.observed)

    def test_a_ready_device_without_the_raw_path_on_is_named(self):
        got = results(record('Maps', READY.format(who='com.apple.MapKit.SnapshotService'), GPU_METAL_SUBSYSTEM))
        self.assertIn(
            '1 without a cut.path-on line: com.apple.MapKit.SnapshotService', got['cut.device-ready'].observed
        )

    def test_a_raised_selector_names_its_class_and_selector(self):
        got = results(record('WidgetRenderer_Default', RAISE))['cut.selector-raised']
        self.assertEqual(got.verdict, FAIL)
        self.assertIn('hostImport:', got.observed)

    def test_deleted_refusal_tags_fail_and_record_refusals_report(self):
        got = results(
            record(
                'PosterBoard',
                'PosterBoard[311] selector.missing: -[SwitchMetalDevice view:] no method',
                REFUSAL_SUBSYSTEM,
            ),
            record('SpringBoard', 'SpringBoard[88] call.buf: a buffer not made by this device', REFUSAL_SUBSYSTEM, 1),
        )
        self.assertEqual(got['cut.deleted-tags'].verdict, FAIL)
        self.assertEqual(got['cut.record-refusals'].verdict, INFO)
        self.assertIn('SpringBoard', got['cut.record-refusals'].observed)

    def test_hw_tests_own_ended_encoders_are_not_a_client_defect(self):
        line = '{who}[51] plan.ended_encoder: a command encoded on an encoder that had ended, not moved inside it (x)'
        hwtest = record('SwitchHWTest', line.format(who='SwitchHWTest'), REFUSAL_SUBSYSTEM)
        self.assertEqual(results(hwtest)['cut.ended-encoder'].verdict, PASS)
        other = record('PosterBoard', line.format(who='PosterBoard'), REFUSAL_SUBSYSTEM)
        self.assertEqual(results(other)['cut.ended-encoder'].verdict, FAIL)

    def test_a_front_end_line_names_a_front_end_loaded_again(self):
        self.assertEqual(results(*clean_boot())['cut.frontend-line'].verdict, PASS)
        line = 'native-metal: no-copy buffer refused: page offset 0x40; region 3: prot 3 share 1'
        got = results(*clean_boot(), record('PosterBoard', line, '', 1))['cut.frontend-line']
        self.assertEqual(got.verdict, FAIL)
        self.assertIn('PosterBoard', got.observed)

    def test_a_kernel_line_of_the_deleted_methods_names_an_old_kernel(self):
        kernel = '/System/Library/Caches/com.apple.kernelcaches/kernel.development'
        hang = 'tegra-gm20b: GPU hang detail: ch 3 (no progress for 2100 ms){wait}; late ch 5: 40/41, moved 12 ms ago'
        current = [
            hang.format(wait=''),
            'tegra-gm20b: paths flags=0x5ced pid=812 submit_jobs=40 set_jobs=40 stale_sets=0',
            'tegra-gm20b: semaphore acquire timed out on SpringBoard (channel 4) 2003 ms after its kick: waits on ch 3',
        ]
        got = results(*clean_boot(), *(record(kernel, line, '', 1) for line in current))['cut.kernel-legacy-line']
        self.assertEqual(got.verdict, PASS)
        old = [
            'tegra-gm20b: paths flags=0x6dfd pid=812 submit_jobs=40 set_jobs=40 stale_sets=0 address_jobs=2',
            'tegra-gm20b: exec by process: backboardd 812, SubmitAddress 3 x mean 20 us (resolve 2, lock 1, exec 15)',
            hang.format(wait=' waits on no other channel'),
            'tegra-gm20b: backboardd: fence 9 still pending after 2000 ms: busy, order wait on chan 5 fence 9 (at 8)',
        ]
        got = results(*clean_boot(), *(record(kernel, line, '', 1) for line in old))['cut.kernel-legacy-line']
        self.assertEqual(got.verdict, FAIL)
        for what in ('address_jobs=', 'SubmitAddress', 'order wait', 'GPU hang detail'):
            self.assertIn(what, got.observed)

    def test_rates_fields_need_a_rates_line(self):
        self.assertEqual(results()['cut.rates-fields'].verdict, NO_DATA)


class CutCrashReports(unittest.TestCase):
    def write(self, directory: Path, name: str, body: dict) -> None:
        header = {'bug_type': '309', 'app_name': name}
        (directory / f'{name}-2026-11-19-091200.ips').write_text(json.dumps(header) + '\n' + json.dumps(body))

    def rows(self, directory: Path) -> dict:
        return {r.check.name: r for r in reports.crash_rows(str(directory))}

    def test_a_raised_raw_selector_and_an_nvk_image_fail(self):
        with tempfile.TemporaryDirectory() as d:
            self.write(Path(d), 'WidgetRenderer', {'asi': {'CoreFoundation': [RAISE]}, 'usedImages': []})
            self.write(Path(d), 'PosterBoard', {'usedImages': [{'name': 'libvulkan_nouveau.dylib'}]})
            got = self.rows(Path(d))
        self.assertEqual(got['crash.gm20b-selector'].verdict, FAIL)
        self.assertIn('WidgetRenderer -[SwitchMetalDevice hostImport:]', got['crash.gm20b-selector'].observed)
        self.assertEqual(got['crash.nvk-image'].verdict, FAIL)
        self.assertIn('PosterBoard', got['crash.nvk-image'].observed)

    def test_other_crashes_leave_the_cut_rows_passing(self):
        with tempfile.TemporaryDirectory() as d:
            other = '-[NSNull length]: unrecognized selector sent to instance 0x1f00'
            self.write(Path(d), 'Maps', {'asi': {'CoreFoundation': [other]}, 'usedImages': [{'name': 'Metal'}]})
            got = self.rows(Path(d))
        self.assertEqual(got['crash.gm20b-selector'].verdict, PASS)
        self.assertEqual(got['crash.nvk-image'].verdict, PASS)


if __name__ == '__main__':
    unittest.main()
