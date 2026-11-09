#!/usr/bin/env python3
"""caspec.py makes iOS 27's Core Animation states (qc-24B119.json) as QuartzCore 27's library declares them.

The journal holds one iOS 27 state, made from its name as drawn; the rest are held to QuartzCore's own metallib (the
tables record each function's declared constants): every state of the raw pack's tiers names functions the library
has, gives each fixed function exactly the constants it declares with their types, and every required one.
"""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import caset  # noqa: E402
import caspec  # noqa: E402

GLASS = (
    'R ok=1 label=com.apple.coreanimation.PBGRABsovXmw_TgbhA3Xhf_Isrc_Isrc samples=1 topo=0 ds=0/0'
    ' c0=80:15:1:1:5:0:1:5:0 c1=115:0:0:1:0:0:1:0:0 c2=55:0:0:1:0:0:1:0:0'
    ' a0=31:0:1:48:1:1 a1=29:16:1:48:1:1 a2=29:24:1:48:1:1 a3=27:32:1:48:1:1'
    ' | v variable_blur_vert_lph  | f glass_background_all_lph  | '
)
DOWNSAMPLE = (
    'R ok=1 label=com.apple.coreanimation.PRGhAXm_Tds4A2Xhf_Isrc samples=1 topo=0 ds=0/0'
    ' c0=115:15:0:1:0:0:1:0:0 c1=115:0:0:1:0:0:1:0:0'
    ' a0=31:0:1:48:1:1 a1=29:16:1:48:1:1 a2=29:24:1:48:1:1 a3=27:32:1:48:1:1'
    ' | v downsample_blur_vert_lph  | f downsample_4_frag_lph  | '
)


def functions(line: str) -> list[tuple[str, dict[str, tuple[int, str]]]]:
    """Give the vertex and fragment function of an R line with their constants: name -> (type, value)."""
    out = []
    for part in (line[line.index(' | v ') + 5 : line.index(' | f ')], line[line.index(' | f ') + 5 :]):
        words = part.rstrip(' |').split()
        consts = {}
        for word in words[1:]:
            name, dtype, value = word.split(':')
            consts[name] = (int(dtype), value)
        out.append((words[0], consts))
    return out


class States27(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tables = caspec.Tables(os.path.join(HERE, 'qc-24B119.json'))

    def test_glass_and_blur_states(self):
        self.assertEqual(caspec.state_line(self.tables, 'PBGRABsovXmw_TgbhA3Xhf_Isrc_Isrc'), GLASS)
        self.assertEqual(caspec.state_line(self.tables, 'PRGhAXm_Tds4A2Xhf_Isrc'), DOWNSAMPLE)

    def test_the_journals_states_are_made_as_drawn(self):
        with open(os.path.join(HERE, '../../../precompile/corpus.log')) as corpus:
            drawn = [caspec.corpus_fields(line) for line in corpus if ' fc_color_opcodes_ext0:' in line]
        drawn = [(name, fields) for name, fields in drawn if name]
        self.assertTrue(drawn)
        for name, fields in drawn:
            self.assertEqual(caspec.state_fields(self.tables, name), fields, name)

    def test_generic_manifest_states_are_in_the_pack(self):
        generic = [m[0] for m in self.tables.t['manifest'] if 'Xg' in m[0]]
        self.assertEqual(len(generic), 19)
        manifest = caset.labels(self.tables, [], ('manifest',))['manifest']
        for name in generic:
            self.assertIn(name, manifest)
            line = caspec.state_line(self.tables, name)
            self.assertIn(' | v fixed_vert_lph_gen fc_base_color:53:00 fc_generic:53:01 ', line)
            self.assertIn(' | f fixed_frag_lph_cpf ', line)
            self.assertNotIn(' a0=', line)

    def test_every_tier_state_is_one_the_library_declares(self):
        declared = self.tables.t['functions']
        checked = 0
        for names in caset.labels(self.tables, []).values():
            for name in names:
                line = caspec.state_line(self.tables, name)
                for function, consts in functions(line):
                    self.assertIn(function, declared, name)
                    wanted = {c: (dtype, required) for c, dtype, required in declared[function]}
                    for const, (dtype, _value) in consts.items():
                        self.assertEqual(wanted.get(const, (None,))[0], dtype, f'{name}: {function} {const}')
                    for const, (_dtype, required) in wanted.items():
                        self.assertTrue(const in consts or not required, f'{name}: {function} lacks {const}')
                checked += 1
        self.assertGreater(checked, 10000)


if __name__ == '__main__':
    unittest.main()
