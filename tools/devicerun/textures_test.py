"""textures.py's checks over refusal lines as kits/SwitchRefusal writes them (`<process>[<pid>] <reason>: <text>`)."""

import unittest

from tools.devicerun import devicerun
from tools.devicerun.cut import REFUSAL_SUBSYSTEM
from tools.devicerun.rules import FAIL, INFO, PASS
from tools.devicerun.textures import TEXTURES

FORM = (
    '{who}[{pid}] tex.surf.form: no 128x1 texture of format 550 and usage 0x1 over IOSurface 41: a texel buffer over a '
    'surface the kernel laid out block-linear, whose bytes an M4 Max reads in order: nil until the driver has its '
    'form: w30r 64x64'
)
ACROSS = (
    '{who}[{pid}] blit.invalid: copyFromTexture:toTexture: between textures of two pixel formats, one held through '
    "planes: its bytes copied: Metal's validation error (MTLBlitCommandEncoder.h)"
)


def record(process: str, message: str, second: int = 0) -> dict:
    return {
        'timestamp': f'2026-11-19 09:12:{second:02d}.000000-0800',
        'bootUUID': '7C1E0A22',
        'processImagePath': '/System/Library/CoreServices/' + process,
        'subsystem': REFUSAL_SUBSYSTEM,
        'category': 'gm20b',
        'messageType': 'Default',
        'eventMessage': message,
    }


def results(*records: dict) -> dict:
    found, _ = devicerun.run_checks(list(records), TEXTURES)
    return {r.check.name: r for r in found}


class Textures(unittest.TestCase):
    def test_a_boot_without_texture_refusals_passes(self):
        got = results()
        self.assertEqual(got['textures.removed-refusal'].verdict, PASS)
        self.assertEqual(got['textures.surf-pack'].verdict, PASS)

    def test_a_refusal_the_fixes_removed_names_an_old_driver(self):
        old = (
            "SpringBoard[88] pass.ycbcr: 64x128 format 500: a turned YCbCr target loaded: no tile mask of its planes' "
            'tiles: the command buffer fails'
        )
        got = results(record('SpringBoard', old))['textures.removed-refusal']
        self.assertEqual(got.verdict, FAIL)
        self.assertIn('a turned YCbCr target loaded', got.observed)

    def test_a_pack_without_the_planes_states_fails(self):
        line = 'PosterBoard[311] tex.surf.pack: no 32x8 texture of format 550 and usage 0x7 over IOSurface 9: nil: (x)'
        self.assertEqual(results(record('PosterBoard', line))['textures.surf-pack'].verdict, FAIL)

    def test_a_form_line_names_an_old_driver_whoever_asked(self):
        got = results(record('SwitchHWTest', FORM.format(who='SwitchHWTest', pid=51)))['textures.removed-refusal']
        self.assertEqual(got.verdict, FAIL)
        self.assertIn('nil until the driver has its form', got.observed)

    def test_cross_format_copies_report_other_processes_only(self):
        got = results(
            record('SwitchHWTest', ACROSS.format(who='SwitchHWTest', pid=51), 1),
            record('Photos', ACROSS.format(who='Photos', pid=77), 3),
        )
        self.assertEqual(got['textures.blit-across'].verdict, INFO)
        self.assertIn('Photos', got['textures.blit-across'].observed)
        self.assertNotIn('SwitchHWTest', got['textures.blit-across'].observed)


if __name__ == '__main__':
    unittest.main()
