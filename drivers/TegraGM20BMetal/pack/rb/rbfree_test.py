"""rb/rbfree.py over RenderBox's own states.

Each line's constant block and attachments are a corpus line's (precompile/corpus.log); a test that changes one says
which field it changes.
"""

import json
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import rbfree  # noqa: E402
import rbspec  # noqa: E402

LIB = '/System/Library/PrivateFrameworks/RenderBox.framework/default.metallib'
FLOAT = 'c0=115:15:0:1:0:0:1:0:0 c1=105:0:0:1:0:0:1:0:0 c2=115:0:0:1:0:0:1:0:0'
SECOND = 'c0=115:0:0:1:0:0:1:0:0 c1=105:0:0:1:0:0:1:0:0 c2=115:15:0:1:0:0:1:0:0'
EIGHT_BIT = 'c0=80:15:0:1:0:0:1:0:0 c1=105:0:0:1:0:0:1:0:0 c2=80:0:0:1:0:0:1:0:0'
XR = 'c0=550:15:0:1:0:0:1:0:0 c1=105:0:0:1:0:0:1:0:0 c2=550:0:0:1:0:0:1:0:0'
XR_17 = 'c0=554:15:0:1:0:0:1:0:0 c1=65:0:0:1:0:0:1:0:0 c2=552:0:0:1:0:0:1:0:0'
COLOR, GRADIENT, IMAGE = 'primitive_color_fragment', 'primitive_gradient_fragment', 'primitive_image_fragment'
MULTIPLY = '470090040000190073f800003d000000'
COPY = '450000000040000073f800006d000000'
# the vertex function each fragment function's corpus lines name, and whether it is given the constant block too
VERTEX = {COLOR: ('primitive__vertex', False), GRADIENT: ('primitive_gradient_vertex', False)}


def line(fragment: str, state: str, colours: str, ds: str = '260/260') -> str:
    vertex, constant = VERTEX.get(fragment, ('primitive_image_vertex', True))
    stage = f'{vertex} lib={LIB} ' + (f'#0:36:{state}' if constant else '')
    return (
        f'R ok=1 label={fragment.removesuffix("_fragment")} samples=1 topo=0 ds={ds} {colours} | v {stage} '
        f'| f {fragment} lib={LIB} #0:36:{state} | '
    )


def with_word(state: str, word: int, value: int) -> str:
    raw = bytearray.fromhex(state)
    raw[4 * word : 4 * word + 4] = value.to_bytes(4, 'little')
    return raw.hex()


class RenderBox27(unittest.TestCase):
    t = rbspec.load(HERE / 'rb-24B119.json')

    def classify(self, fragment: str, state: str, colours: str = FLOAT, ds: str = '260/260') -> tuple[int, str]:
        return rbfree.classify(self.t, line(fragment, state, colours, ds))

    def test_each_separable_mode_takes_core_animations_program_of_its_formula(self):
        for fragment, state, colours, program in (
            (COLOR, '850000000040190073f800006d000000', FLOAT, 'mpy'),
            (GRADIENT, '860080030040190073f800006d000000', FLOAT, 'mpy'),
            (IMAGE, MULTIPLY, FLOAT, 'mpy'),
            (IMAGE, '470411000000190050f8000021000000', EIGHT_BIT, 'mpy'),
            (IMAGE, '8700100000401a0073f800006d000000', FLOAT, 'ovl'),
            (IMAGE, '4700100000001b0073f800002d000000', FLOAT, 'drk'),
            (IMAGE, '4700100400401c0073f800006d000000', FLOAT, 'ltn'),
            (GRADIENT, '860080030040200073f800006d000000', FLOAT, 'hlt'),
            (IMAGE, '470010040000200073f800002d000000', FLOAT, 'hlt'),
        ):
            with self.subTest(fragment=fragment, state=state):
                self.assertEqual(self.classify(fragment, state, colours), (3, program))

    def test_the_form_clears_the_fragments_blend_and_destination_read_and_not_the_vertex_constant(self):
        tag, why = rbfree.tag(self.t, line(IMAGE, MULTIPLY, FLOAT))
        self.assertEqual((tag, why), (' rung=3 free=3:0:mpy;f/#0:36:470090040000190073f800001c000000', ''))

    def test_a_fixed_function_mode_takes_its_pipeline_blend_on_rung_2(self):
        normal = with_word(MULTIPLY, 1, 0x00020000)  # the mode field: normal
        tag, _ = rbfree.tag(self.t, line(IMAGE, normal, FLOAT))
        self.assertEqual(tag, ' rung=2 free=2:0:;f/#0:36:470090040000020073f800001c000000,c0=115:15:1:1:5:0:1:5:0')

    def test_a_mode_saturating_the_source_alpha_is_left_on_a_float_target_unless_extended(self):
        normal = with_word(with_word(MULTIPLY, 1, 0x00020000), 3, 0x39)  # normal, extended_color cleared
        self.assertEqual(self.classify(IMAGE, normal), (0, 'saturates a source alpha the float target does not clamp'))

    def test_blends_not_linear_in_coverage_keep_the_storage_form(self):
        for fragment, state, colours, why in (
            (COLOR, COPY, FLOAT, rbfree.KEPT['copy']),
            (GRADIENT, '8600800300402b0073f800006d000000', FLOAT, rbfree.KEPT['plus_saturated']),
            (GRADIENT, '8600800300402c0073f800006d000000', FLOAT, rbfree.KEPT['plus_darker']),
            (IMAGE, '470010040040014073f800006d000000', FLOAT, 'custom blend function'),
            (IMAGE, '47041100008033005af8000021000000', SECOND, 'maximum_red is not linear in coverage'),
        ):
            with self.subTest(state=state):
                self.assertEqual(self.classify(fragment, state, colours), (0, why))

    def test_targets_and_reads_the_rop_cannot_stand_in_for(self):
        own_coverage = FLOAT.replace('c1=105:0:', 'c1=105:4:')  # the coverage target written
        for fragment, state, colours, why in (
            (IMAGE, '4704110000001a0026fa000025000000', XR, 'blended colour 550 not RGBA8, BGRA8 or RGBA16Float'),
            (IMAGE, '470010040080190073f800002d000000', SECOND, 'blended colour not the first'),
            (COLOR, '850000000040190073f800006d000000', own_coverage, 'reads the coverage target it writes'),
            (IMAGE + '_depth', MULTIPLY, FLOAT, 'draws through its tests=1..4 variants'),
        ):
            with self.subTest(why=why):
                self.assertEqual(self.classify(fragment, state, colours), (0, why))

    def test_a_depth_exporting_function_takes_its_form_with_no_depth_or_stencil_attachment(self):
        self.assertEqual(self.classify(IMAGE + '_depth', MULTIPLY, ds='0/0'), (3, 'mpy'))

    def test_a_state_reading_only_the_coverage_target_names_no_form_and_no_reason(self):
        covered = 'c0=115:15:1:1:5:0:1:5:0 c1=105:0:0:1:0:0:1:0:0 c2=115:0:0:1:0:0:1:0:0'
        self.assertEqual(self.classify(IMAGE, '470010000040020073f800004c000000', covered), (0, ''))


class RenderBox17(unittest.TestCase):
    t = rbspec.load(HERE / 'rb-21H461.json')
    COPY = '07100048010000002a12740055000000'
    # its mode field (word 0, bits 6-11) set to multiply
    MULTIPLY = with_word(COPY, 0, 0x48001007 & ~(63 << 6) | 23 << 6)

    def test_its_copy_keeps_the_storage_form(self):
        self.assertEqual(rbfree.classify(self.t, line(IMAGE, self.COPY, XR_17)), (0, rbfree.KEPT['copy']))

    def test_a_discarding_function_beside_depth_and_stencil_draws_its_variants(self):
        self.assertEqual(
            rbfree.classify(self.t, line(IMAGE, self.MULTIPLY, FLOAT)), (0, 'draws through its tests=1..4 variants')
        )
        self.assertEqual(rbfree.classify(self.t, line(IMAGE, self.MULTIPLY, FLOAT, ds='0/0')), (3, 'mpy'))


class Tagger(unittest.TestCase):
    def test_the_split_tags_the_ipsws_renderbox_lines_by_its_quartzcore_digest(self):
        with open(HERE / 'rb-24B119.json') as f:
            digest = json.load(f)['quartzcore_metallib_sha256']
        tagged, why = rbfree.tagger(HERE, digest)
        served, kept = line(IMAGE, MULTIPLY, FLOAT), line(COLOR, COPY, FLOAT)
        self.assertIn(' rung=3 free=3:0:mpy;', tagged(served).split(' samples=')[0])
        self.assertEqual(tagged(kept), kept)
        self.assertEqual(why, {'form on rung 3': 1, rbfree.KEPT['copy']: 1})
        self.assertIsNone(rbfree.tagger(HERE, '0' * 64))


if __name__ == '__main__':
    unittest.main()
