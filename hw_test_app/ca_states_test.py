"""ca_states.py over the corpus and the selected IPSW's QuartzCore: each build's sweep asks for its own states only."""

import os
import unittest
from pathlib import Path

from hw_test_app import ca_states

LINE = (
    'R ok=1 label=com.apple.coreanimation.PBGRAXm_A2Xhf samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0 '
    '| v {vertex} lib=/System/Library/Frameworks/QuartzCore.framework/default.metallib {constants} '
    '| f {fragment}  | '
)


class States(unittest.TestCase):
    def setUp(self) -> None:
        self.build = os.environ['CA_BUILD']
        self.corpus = Path(os.environ['CA_CORPUS']).read_text()
        self.library = Path(os.environ['CA_LIBRARY']).read_bytes()
        self.tables = ca_states.caspec.Tables(os.environ['CA_TABLES'])

    def test_only_ios_seventeen_keeps_its_stroke_states(self) -> None:
        kept = ca_states.states(self.corpus, self.library, self.tables)
        strokes = [line for line in kept if '| v path_stroke_' in line]
        if self.build == '21H461':
            self.assertGreater(len(strokes), 0)
        else:
            self.assertEqual(strokes, [], 'iOS 27 has no path_stroke_* functions')
        self.assertGreater(len(kept), 600 if self.build == '21H461' else 15)

    def test_a_line_another_build_journaled_is_not_this_builds_state(self) -> None:
        line = LINE.format(vertex='std_vert1_lph', constants='', fragment='luma_boost_lph')
        tagged = line.replace(' samples=', ' os=21H461 samples=')
        self.assertTrue(ca_states.tagged(tagged, '21H461'))
        self.assertFalse(ca_states.tagged(tagged, '24B119'))
        self.assertTrue(ca_states.tagged(tagged.replace('os=21H461', 'os=21H461,24B119'), '24B119'))
        self.assertFalse(ca_states.tagged(line, '24B119'))

    def test_a_constant_the_function_does_not_declare_is_another_builds_state(self) -> None:
        declared = {'std_vert1_lph': {'fc_generic'}, 'luma_boost_lph': set()}
        own = LINE.format(vertex='std_vert1_lph', constants='fc_generic:53:00', fragment='luma_boost_lph')
        other = LINE.format(vertex='std_vert1_lph', constants='fc_perspective:53:00', fragment='luma_boost_lph')
        missing = LINE.format(vertex='path_stroke_line_vert_lph', constants='', fragment='luma_boost_lph')
        self.assertTrue(ca_states.made_here(own, declared))
        self.assertFalse(ca_states.made_here(other, declared))
        self.assertFalse(ca_states.made_here(missing, declared))

    def test_another_builds_constants_for_a_name_are_not_this_builds_state(self) -> None:
        kept = {line.split()[2] for line in ca_states.states(self.corpus, self.library, self.tables)}
        ios17 = 'label=com.apple.coreanimation.PBGRABxorXm_TcimA2Xhf_IsrcM1'
        ios27 = 'label=com.apple.coreanimation.PBGRABacpXm_TgsaA2Xhf_Isrc'
        self.assertEqual(ios17 in kept, self.build == '21H461')
        self.assertEqual(ios27 in kept, self.build == '24B119')


if __name__ == '__main__':
    unittest.main()
