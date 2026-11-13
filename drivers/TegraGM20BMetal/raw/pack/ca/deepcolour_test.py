"""deepcolour.py: the colour precision this device's QuartzCore names Core Animation's states with, on both IPSWs."""

import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import caset
import caspec
import deepcolour
from packfile import fnv
from rawpack import state_key

HERE = os.path.dirname(os.path.abspath(__file__))
STAGER = Path(HERE).parents[2] / 'stage-tegra-gm20b-metal.py'
# A state iOS 27's backboardd drew on the device, with its key (T1 18:06:13 "keyline draw"); the manifest names it
# PBGRABsovXmw_A3Xhfc
DRAWN_27 = ('PBGRABsovXmw_A3Xhf', 0x72A38B147949318A)


def key_hash(tables: caspec.Tables, name: str) -> int:
    return fnv(state_key(caspec.state_line(tables, name)).encode())


class Preferences(unittest.TestCase):
    def test_the_stager_turns_deep_colour_off(self):
        self.assertIs(deepcolour.cif10(STAGER), False)

    def stager(self, text: str) -> Path:
        path = Path(tempfile.mkdtemp()) / 'stager.py'
        path.write_text(text)
        return path

    def test_mobilegestalt_decides_without_the_preference_off(self):
        self.assertIsNone(deepcolour.cif10(self.stager("PREFS = {'CAEnableDeepFramebuffer': True}\n")))
        self.assertIsNone(deepcolour.cif10(self.stager("PREFS = {'CAEnableIOSurfaceCompression': False}\n")))

    def test_a_stager_without_preferences_is_refused(self):
        with self.assertRaises(ValueError):
            deepcolour.cif10(self.stager('OTHER = {}\n'))


class Forms27(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.t = caspec.Tables(os.path.join(HERE, 'qc-24B119.json'))

    def test_a_state_with_no_image_is_asked_without_c(self):
        self.assertEqual(deepcolour.asked(self.t, 'PBGRABsovXmw_A3Xhfc', False), ['PBGRABsovXmw_A3Xhf'])

    def test_its_form_is_the_state_the_device_drew(self):
        name, drawn = DRAWN_27
        (form,) = deepcolour.asked(self.t, 'PBGRABsovXmw_A3Xhfc', False)
        self.assertEqual(form, name)
        self.assertEqual(key_hash(self.t, form), drawn)
        self.assertIn(' | f fixed_frag_lph_cph ', caspec.state_line(self.t, form))

    def test_an_image_can_still_set_c(self):
        self.assertEqual(
            deepcolour.asked(self.t, 'PBGRABsovXmw_TimgA3Xhfcu_Isrc', False),
            ['PBGRABsovXmw_TimgA3Xhfu_Isrc', 'PBGRABsovXmw_TimgA3Xhfcu_Isrc'],
        )

    def test_generic_states_keep_their_name(self):
        self.assertEqual(deepcolour.asked(self.t, 'PBGRAXm_A2Xghfc', False), ['PBGRAXm_A2Xghfc'])

    def test_unknown_answer_gives_both_and_deep_colour_gives_c(self):
        self.assertEqual(
            deepcolour.asked(self.t, 'PBGRABsovXmw_A3Xhfc', None), ['PBGRABsovXmw_A3Xhfc', 'PBGRABsovXmw_A3Xhf']
        )
        self.assertEqual(deepcolour.asked(self.t, 'PBGRABsovXmw_A3Xhf', True), ['PBGRABsovXmw_A3Xhfc'])

    def test_the_manifest_tier_holds_apples_names_and_the_asked_forms(self):
        manifest = caset.labels(self.t, [], ('manifest',), False)['manifest']
        for name in ('PBGRABsovXmw_A3Xhf', 'PBGRABsovXmw_A3Xhfc', 'PBGRABsovXmw_TimgA3Xhfc_Isrc'):
            self.assertIn(name, manifest)
        self.assertIn('PBGRABsovXmw_TimgA3Xhf_Isrc', manifest)

    def test_a_fragment_on_bgra8_drops_the_extended_range_flag(self):
        blend = caset.labels(self.t, [], ('blend',), False)['blend']
        self.assertIn('PBGRABsovXmw_TcimA3Xhfc_Iara', blend)
        self.assertNotIn('PBGRABsovXmw_TcimA3Xhfcx_Iara', blend)
        self.assertTrue(
            all('x' not in caset.split(n)[1].partition('_')[0].rpartition('X')[2] for n in blend if n[1:5] == 'BGRA')
        )

    def test_every_form_differs_from_its_name_in_c_alone(self):
        for name, *_ in self.t.t['manifest']:
            if name[0] != 'P':
                continue
            s = caspec.decode(self.t, name)
            for form in deepcolour.asked(self.t, name, False):
                f = caspec.decode(self.t, form)
                self.assertLessEqual(f.flags ^ s.flags, {'c'}, (name, form))
                self.assertEqual((f.T, f.pflags, f.fragB), (s.T, s.pflags, s.fragB), (name, form))


class Forms17(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.t = caspec.Tables(os.path.join(HERE, 'qc-21H461.json'))

    def full_precision_without_images(self, name: str) -> bool:
        s = caspec.decode(self.t, name)
        return s.flag('c') and s.flag('h') and not s.flag('g') and not self.t.images(s.T)

    def test_ios_17s_states_are_asked_without_c_too(self):
        name = next(n for n, *_ in self.t.t['manifest'] if n[0] == 'P' and self.full_precision_without_images(n))
        (form,) = deepcolour.asked(self.t, name, False)
        self.assertFalse(caspec.decode(self.t, form).flag('c'))
        self.assertIn(' | f fixed_frag_lph_cph ', caspec.state_line(self.t, form))


if __name__ == '__main__':
    unittest.main()
