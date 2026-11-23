"""nakstat's decode and classes against a program NAK printed (QuartzCore 27's fixed_frag_lph_cpf image draw)."""

import unittest
from pathlib import Path

from tools.gpu.maxwell import isa, nakstat

FIXTURE = Path(__file__).with_name('testdata') / 'image_draw_fragment.hex'


class NakstatTest(unittest.TestCase):
    def setUp(self):
        self.code = isa.from_hex_dump(FIXTURE.read_text())

    def test_names_match_nak(self):
        names = [isa.name(w) for w in isa.instructions(self.code)]
        self.assertEqual(len(names), 63)
        self.assertNotIn('UNKNOWN', names)
        self.assertEqual(names.count('F2F_reg'), 20)
        self.assertEqual(names.count('HADD2_reg'), 3)
        self.assertEqual(names[-1], 'NOP')

    def test_classes(self):
        c = nakstat.classify(self.code)
        self.assertEqual((c['tex'], c['conv'], c['half'], c['ldc'], c['guard']), (1, 20, 3, 1, 4))
        self.assertEqual(sum(v for k, v in c.items() if k != 'guard'), 63)

    def test_occupancy_steps(self):
        self.assertEqual([nakstat.warps_per_sm(g) for g in (24, 32, 40, 48, 64, 168)], [64, 64, 48, 40, 32, 12])

    def test_state_key(self):
        line = 'R ok=1 label=x samples=1 topo=0 ds=0/0 c0=80:15 a0=31:0 | v vf  | f ff  | '
        self.assertEqual(nakstat.state_key(line), 'samples=1 topo=0 ds=0/0 c0=80:15 a0=31:0  | v vf  | f ff')

    def test_a_framework_stage_of_no_constants_keeps_its_space_before_the_framework(self):
        # TegraGM20BMetalPipelineKey.h pipelineKey trims the key's end alone: " | f <function> " then " | l <framework>"
        lib = ' lib=/System/Library/PrivateFrameworks/MediaCoreUI.framework/default.metallib'
        line = f'R ok=1 label=- samples=1 topo=0 ds=0/0 c0=80:15 | v pinch_vertex{lib}  | f pinch_fragment{lib}  | '
        want = 'samples=1 topo=0 ds=0/0 c0=80:15  | v pinch_vertex  | f pinch_fragment  | l MediaCoreUI'
        self.assertEqual(nakstat.state_key(line), want)

    def test_a_variant_keys_as_its_framework_state(self):
        # the pack holds a variant as a variant record under its state's own key
        lib = ' lib=/System/Library/PrivateFrameworks/RenderBox.framework/default.metallib '
        line = f'R ok=1 label=- samples=1 topo=0 c0=80:15 | v plane__vertex{lib} | f uber{lib}#0:36:01 | wtex=1:23'
        want = 'samples=1 topo=0 c0=80:15  | v plane__vertex  | f uber #0:36:01 | l RenderBox'
        self.assertEqual(nakstat.state_key(line), want)

    def test_a_stage_keys_by_its_library_function_and_a_linked_function_by_its_specialized_name(self):
        rb = ' lib=/System/Library/PrivateFrameworks/RenderBox.framework/default.metallib'
        ir = ' lib=/System/Library/PrivateFrameworks/IconRendering.framework/default.metallib'
        line = (
            f'R ok=1 label=custom_effect:clampToEdges_v1 os=24B119 samples=1 topo=0 ds=260/260 c0=550:15:1:1:5:0:1:5:0'
            f' | v custom_effect_vertex{rb}  | f custom_effect_fragment{rb} as=ffgea5 #0:36:6504'
            f' | fp clampToEdges_v1{ir} as=custom_fn #1:33:00000000 | cubic s0:1110n t0:80 | '
        )
        want = (
            'samples=1 topo=0 ds=260/260 c0=550:15:1:1:5:0:1:5:0  | v custom_effect_vertex  | f custom_effect_fragment'
            ' #0:36:6504 | fp clampToEdges_v1 as=custom_fn #1:33:00000000 | l RenderBox'
        )
        self.assertEqual(nakstat.state_key(line), want)


if __name__ == '__main__':
    unittest.main()
