"""pack_image.py's boot-time row over lines in the kernel's, the Metal driver's and backboardd's formats."""

import unittest
from datetime import datetime, timedelta

from tools.devicerun import devicerun, pack_image, springboard
from tools.devicerun.rules import INFO, NO_DATA

BOOT = '2693043C-362E-46C3-A562-EB07470C449B'
WALL = datetime(2026, 11, 25, 19, 46, 24)

STREAM = (
    'tegra-gm20b: pack image: tier 0 133 MiB for boot, open 700 ms after the build began (tables read), streamed '
    '7800 ms after that (pagein requests 133, copy 80 ms, big-page bytes 0); {jobs} jobs waited on it on the GPU, the '
    'first submitted {ahead} ms before its end'
)


def record(process: str, subsystem: str, message: str, uptime: float) -> dict:
    return {
        'timestamp': (WALL + timedelta(seconds=uptime)).strftime('%Y-%m-%d %H:%M:%S.%f') + '+0000',
        'bootUUID': BOOT,
        'processImagePath': f'/usr/libexec/{process}',
        'subsystem': subsystem,
        'messageType': 'Default',
        'eventMessage': message,
        'machTimestamp': int(uptime * springboard.MACH_HZ),
    }


def logo(uptime: float) -> dict:
    return record('backboardd', 'com.apple.BackBoard', 'Dismissing boot logo (pid:30)', uptime)


def kernel(message: str, uptime: float) -> dict:
    return {**record('kernel', '', message, uptime), 'processImagePath': '/kernel.development'}


def launchd(uptime: float) -> dict:
    return kernel('SwitchBootSteps: before launchd (ms): overlay 372, prefs 12', uptime)


def stream(uptime: float, jobs: int = 0, ahead: int = 0) -> dict:
    return kernel(STREAM.format(jobs=jobs, ahead=ahead), uptime)


def mapped(uptime: float) -> dict:
    return record(
        'backboardd',
        'com.apple.Metal',
        'SwitchMetal: backboardd: pack window mapped in 900 us (133 MiB tier 0)',
        uptime,
    )


def waited(ms: int, uptime: float) -> dict:
    message = f'SwitchMetal: backboardd: waited {ms} ms at device open for the pack image'
    return record('backboardd', 'com.apple.Metal', message, uptime)


def row(*records: dict):
    results, _ = devicerun.run_checks(list(records), pack_image.PACK_IMAGE + springboard.SPRINGBOARD)
    (only,) = pack_image.pack_image_rows({r.check.name: r for r in results})
    return only


class PackImageBoot(unittest.TestCase):
    def test_a_boot_reads_each_step_in_uptime_with_the_open_and_start_counted_back_from_the_stream_line(self):
        got = row(launchd(14.6), stream(23.1, jobs=2, ahead=1200), mapped(16.0), logo(35.4))
        self.assertEqual(got.verdict, INFO)
        self.assertIn('launchd 14.6 s', got.observed)
        self.assertIn(
            'pack image began 14.6 s, open 15.3 s, streamed 23.1 s (133 MiB, for boot; 2 jobs waited on '
            'the GPU, the first 1200 ms before the end)',
            got.observed,
        )
        self.assertIn('backboardd opened 16.0 s, waited < 1 ms', got.observed)
        self.assertIn('boot time 35.4 s', got.observed)

    def test_backboardds_wait_is_the_one_named_before_its_open(self):
        got = row(stream(37.0), waited(7138, 29.8), mapped(29.8), logo(41.0))
        self.assertIn('backboardd opened 29.8 s, waited 7138 ms', got.observed)
        self.assertIn('0 jobs waited on the GPU)', got.observed)
        self.assertIn('no launchd line', got.observed)

    def test_a_boot_without_its_kernel_line_says_so(self):
        got = row(mapped(16.0), logo(35.4))
        self.assertIn('no pack image line', got.observed)
        self.assertIn('boot time 35.4 s', got.observed)

    def test_no_pack_lines_is_no_data(self):
        self.assertEqual(row(logo(35.4)).verdict, NO_DATA)

    def test_the_row_is_among_devicerun_s_groups(self):
        groups = dict(devicerun.evaluate_all([stream(23.1), mapped(16.0), logo(35.4)], None))
        self.assertEqual([r.check.name for r in groups['pack-image-boot']], ['boot.pack-image-time'])
        self.assertEqual(groups['pack-image-boot'][0].verdict, INFO)


if __name__ == '__main__':
    unittest.main()
