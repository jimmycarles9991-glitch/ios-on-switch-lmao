"""fetchfree.py: which fetching Core Animation states get a fetch-free form, and the edits that make it."""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import caset
import caspec
import fetchfree

HERE = os.path.dirname(os.path.abspath(__file__))


class Forms27(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.t = caspec.Tables(os.path.join(HERE, 'qc-24B119.json'))

    def form(self, name: str) -> tuple[int, str]:
        return fetchfree.classify(self.t, name, caspec.state_line(self.t, name))

    def test_a_separable_mode_on_bgra8_takes_its_iterated_program(self):
        self.assertEqual(self.form('PBGRAXm_TcimBmpyA2Xhf_Isr1'), (3, 'mpy'))
        self.assertEqual(self.form('PBGRAXm_TcimBlltA2Xhf_Isr1'), (3, 'llt'))

    def test_colour_burn_stays_on_the_storage_form(self):
        self.assertEqual(self.form('PBGRAXm_TcimBcbnA2Xhf_IgrnCcl'), (0, 'cbn beyond the bound on the host gate'))

    def test_the_colour_matrix_takes_the_macros_program(self):
        self.assertEqual(self.form('PBGRAXm_TimgBvcmA2Xhfn_Isrc'), (3, 'vcm'))
        self.assertEqual(self.form('PBGRAXmw_TdfgBvcmA3Xhfs_Isrc'), (0, 'HDR scale or EDR factor'))

    def test_a_colour_matrix_texture_function_keeps_the_blends_beyond_the_bound(self):
        why = 'pld beyond the bound on the host gate with its texture function'
        self.assertEqual(self.form('PBGRAXm_Tc33BpldA2Xhf_IsrcM1'), (0, why))
        self.assertEqual(self.form('PBGRAXm_Tc4bBvcmA2Xhf_Isrc'), (3, 'vcm'))
        self.assertEqual(self.form('PBGRAXm_Tc33BpinA2Xhf_Isrc'), (3, 'pin'))

    def test_a_clear_takes_the_rop_blend(self):
        self.assertEqual(self.form('PBGRAXmw_BclrA3Xhf'), (2, ''))

    def test_what_has_no_form(self):
        self.assertEqual(self.form('PBGRAXm_BbcmA2Xhf')[0], 0)  # reads a second attachment
        self.assertEqual(self.form('PBGRAXmw_TcimBdsoA3Xhfs_Ialp'), (0, 'HDR scale or EDR factor'))
        self.assertEqual(self.form('PBGRAXm_TcimBaddA2S1Xhfu_Ialp'), (0, 'blended colour not 8-bit unorm'))
        self.assertEqual(self.form('PBGRAXm_TimgA2Xhfn_Isrc'), (0, 'no fragment blend'))

    def test_what_the_host_gate_has_not_shown_within_the_bound_has_no_form(self):
        self.assertEqual(self.form('PBGRAXm_TkfhBpldA2Xhf_Irsd'), (0, 'kfh discards every lane on the host gate'))
        self.assertEqual(self.form('PBGRAXmw_TimgBlltA3Xhfcu_IsrcN3Ocsmtsdnlnlnl'), (0, 'colour opcodes'))
        self.assertEqual(self.form('PBGRAXm_TcimBdivA2Xhf_Isrc'), (0, 'div beyond the bound on the host gate'))

    def test_another_builds_line_of_the_name_has_no_form(self):
        name = 'PBGRAXm_TcimBmpyA2Xhf_Isr1'
        t17 = caspec.Tables(os.path.join(HERE, 'qc-21H461.json'))
        line = caspec.state_line(t17, name)
        self.assertEqual(fetchfree.classify(self.t, name, line), (0, "constants not this QuartzCore's of its name"))

    def test_the_edits_set_the_blend_to_none_and_rung_2_lifts_it(self):
        line = caspec.state_line(self.t, 'PBGRAXmw_BclrA3Xhf')
        tag, why = fetchfree.tag(self.t, 'PBGRAXmw_BclrA3Xhf', line)
        self.assertEqual(why, '')
        none = f'{self.t.none_blend:02x}'
        self.assertEqual(tag, f' free=2:0:;fc_blend_function:49:{none},fc_dest_count:49:00,c0=80:15:1:0:0:0:0:0:0')
        lifted = caspec.state_line(self.t, 'PBGRABclrXmw_A3Xhf')
        self.assertIn(' c0=80:15:1:0:0:0:0:0:0 ', lifted)

    def test_the_corpus_line_carries_the_rung_and_the_form(self):
        line = caspec.state_line(self.t, 'PBGRAXm_TcimBmpyA2Xhf_Isr1')
        why: dict[str, int] = {}
        tagged = caset.with_fetch_rung(self.t, line, why)
        self.assertIn(' rung=3 free=3:0:mpy;fc_blend_function:49:', tagged)
        self.assertEqual(why, {'rung 3: form on rung 3': 1})


class Forms17(unittest.TestCase):
    def test_ios17_states_are_classified_with_its_tables(self):
        t = caspec.Tables(os.path.join(HERE, 'qc-21H461.json'))
        name = 'PBGRAXm_TcimBmpyA2Xhf_Isr1'
        try:
            line = caspec.state_line(t, name)
        except (KeyError, IndexError, ValueError):
            self.skipTest('QuartzCore 21H461 makes no such state')
        self.assertEqual(caset.fetch_rung(t, name), 3)
        self.assertIn(fetchfree.classify(t, name, line)[0], (0, 3))

    def test_plus_darker_stays_on_the_storage_form_on_21h461(self):
        t = caspec.Tables(os.path.join(HERE, 'qc-21H461.json'))
        name = 'PBGRAXm_TimgBpdiA2Xhf_Isrc'
        self.assertEqual(
            fetchfree.classify(t, name, caspec.state_line(t, name)), (0, 'pdi beyond the bound on the host gate')
        )


if __name__ == '__main__':
    unittest.main()
