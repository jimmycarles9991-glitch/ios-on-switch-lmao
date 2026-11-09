"""lockscreen.py's checks over boot 15's lock-screen lines (11-08, iOS 27)."""

import unittest

from tools.devicerun import devicerun
from tools.devicerun.lockscreen import LOCKSCREEN
from tools.devicerun.rules import FAIL, INFO, NO_DATA, PASS

REFUSAL = 'com.brunostjohn.switch.refusal'
RAW = 'com.brunostjohn.switch.TegraGM20BMetal'
MERCURY_M2V = (
    'MercuryPosterExtension[467] cb.m2v: command buffer on m2v (encode): a storage buffer bound at an offset that is '
    'not a multiple of 16 [first predicate enc.ubo]'
)
CAMERA_M2V = (
    "cameracaptured[64] cb.m2v: command buffer on m2v (compute): a kernel's buffer bound at an offset that is not a "
    'multiple of 16 [first predicate compute]'
)
WATCHDOG = (
    '[extension<com.apple.MercuryPoster:467] Provision violated for watchdog scene-create: '
    '<FBSProcessResourceProvision: 0xd281501e0; allowance: <; FBSProcessResourceAllowance; type: realTime; '
    'timeValue: 10.0s>; violated: YES>'
)
ABORT = 'raw: waitUntilScheduled of command buffer 0xda48d2400 not done after 20 s: abort() for the stack'
BEHIND_M2V = (
    'raw: raw command buffer waiting for the m2v work before it, command buffer 0x7bf660400 not done after 20 s: '
    'abort() for the stack'
)
PORTRAIT = 'currentTallest: 0.600000, upper: 0.615638, lower: 0.224219'
LANDSCAPE = 'currentTallest: 0.600000, upper: 0.439949, lower: 0.199219'
ADAPTIVE = (
    '[Adaptive Time] Calculated timeHeight: 132.800000 from honorsSalientContent: false, posterDerivedTimeMaxY: '
    '0.000000, timeBottomFromList: 1016.000000, preferredSalientContent: <private>, screenMax: 1024.000000, '
    'salientSpaceLeft: -96.800000'
)
ORIENTATION = (
    '<PRRenderer 0xb9cc4c900> Device Orientation has updated from UIInterfaceOrientationLandscapeRight -> '
    'UIInterfaceOrientationLandscapeLeft'
)


def record(process: str, message: str, subsystem: str = '', second: int = 0) -> dict:
    return {
        'timestamp': f'2026-11-08 19:21:{second:02d}.000000-0800',
        'bootUUID': '31AB01C1',
        'processImagePath': '/usr/libexec/' + process,
        'subsystem': subsystem,
        'category': '',
        'messageType': 'Error',
        'eventMessage': message,
    }


def results(*records: dict) -> dict:
    found, _ = devicerun.run_checks(list(records), LOCKSCREEN)
    return {r.check.name: r for r in found}


class Lockscreen(unittest.TestCase):
    def test_boot_15_fails_the_lock_screen_checks(self):
        got = results(
            record('MercuryPosterExtension', MERCURY_M2V, REFUSAL),
            record('PosterBoard', WATCHDOG),
            record('backboardd', ABORT, RAW),
            record('backboardd', BEHIND_M2V, RAW, second=2),
            record('MercuryPosterExtension', PORTRAIT),
            record('MercuryPosterExtension', LANDSCAPE, second=1),
        )
        self.assertEqual(got['lockscreen.m2v'].verdict, FAIL)
        self.assertIn('enc.ubo', got['lockscreen.m2v'].observed)
        self.assertEqual(got['lockscreen.scene-watchdog'].verdict, FAIL)
        self.assertIn('com.apple.MercuryPoster', got['lockscreen.scene-watchdog'].observed)
        self.assertEqual(got['lockscreen.display-abort'].verdict, FAIL)
        self.assertIn('2 lines', got['lockscreen.display-abort'].observed)
        self.assertIn('waiting for the m2v', got['lockscreen.display-abort'].observed)
        self.assertEqual(got['lockscreen.hero-fits-time'].verdict, INFO)
        self.assertIn('0.439949', got['lockscreen.hero-fits-time'].observed)

    def test_a_clean_lock_screen_passes(self):
        got = results(
            record('cameracaptured', CAMERA_M2V, REFUSAL),
            record('MercuryPosterExtension', PORTRAIT),
        )
        self.assertEqual(got['lockscreen.m2v'].verdict, PASS)
        self.assertEqual(got['lockscreen.scene-watchdog'].verdict, PASS)
        self.assertEqual(got['lockscreen.display-abort'].verdict, PASS)
        self.assertEqual(got['lockscreen.hero-fits-time'].verdict, INFO)

    def test_no_celosia_line_is_no_data(self):
        self.assertEqual(results()['lockscreen.hero-fits-time'].verdict, NO_DATA)

    def test_reports_name_what_the_host_and_poster_saw(self):
        got = results(
            record('SpringBoard', ADAPTIVE),
            record('SpringBoard', 'Poster scenes FENCED update for reasons: MaglevDynamicOrientationUpdate'),
            record('MercuryPosterExtension', ORIENTATION),
        )
        self.assertEqual(got['lockscreen.adaptive-time'].verdict, INFO)
        self.assertIn('-96.8', got['lockscreen.adaptive-time'].observed)
        self.assertEqual(got['lockscreen.host-rotation'].verdict, INFO)
        self.assertIn('LandscapeLeft', got['lockscreen.poster-orientation'].observed)


if __name__ == '__main__':
    unittest.main()
