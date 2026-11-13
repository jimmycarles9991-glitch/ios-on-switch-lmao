"""What derive.py adds to a corpus."""

import unittest

from tools.gpu.shaders import derive, plan
from tools.gpu.shaders.journal import parse_kernel, parse_render, parse_stage
from tools.gpu.shaders.metallib import FRAGMENT, KERNEL, Constant, Function

LIB = '/System/Library/Frameworks/X.framework/default.metallib'
FIELDS = 'form=d opt=0 max=0 tgm=1 icb=0 depth=1 abf=0 mut=0000000000000000000000000000000'


def _render(target: str, vertex: str, fragment: str) -> str:
    return f'R ok=1 label=- {target} | v {vertex} lib={LIB} | f {fragment} lib={LIB} | '


class KernelsTest(unittest.TestCase):
    def test_unseen_library_gets_its_plain_kernels_only(self):
        index = {
            LIB: [
                Function('plain', KERNEL, ()),
                Function('needs', KERNEL, (Constant('n', 33, 0, True),)),
                Function('frag', FRAGMENT, ()),
            ]
        }
        self.assertEqual(derive.kernels(index, []), [f'C ok=1 form=f opt=0 | k plain lib={LIB} | '])

    def test_seen_library_lends_its_fields_and_constants(self):
        index = {LIB: [Function('seen', KERNEL, ()), Function('new', KERNEL, (Constant('a', 53, 0, True),))]}
        corpus = [f'C ok=1 {FIELDS} label=x | k seen lib={LIB} #0:53:00 #127:33:c80e0000 | '] * 3
        lines = derive.kernels(index, corpus)
        self.assertEqual(lines, [f'C ok=1 {FIELDS} | k new lib={LIB} #0:53:00 #127:33:c80e0000 | '])

    def test_kernel_specialized_but_never_made_gets_a_pipeline(self):
        index = {LIB: [Function('k', KERNEL, (Constant('a', 53, 0, True),))]}
        corpus = [f'S ok=1 lib={LIB} name=k k=#0:53:01 | ']
        self.assertEqual(derive.kernels(index, corpus), [f'C ok=1 form=f opt=0 | k k lib={LIB} #0:53:01 | '])


class RenderTest(unittest.TestCase):
    def test_targets_cross_programs_with_their_fragments_targets(self):
        a = parse_render(_render('samples=1 c0=80:15:0:1:0:0:1:0:0', 'v1', 'f'))
        b = parse_render(_render('samples=1 c0=115:15:0:1:0:0:1:0:0', 'v2', 'f'))
        derived = {(r.vertex.name, r.target) for r in derive.targets([a, b])}
        self.assertEqual(derived, {('v1', b.target), ('v2', a.target)})

    def test_neighbours_change_one_constant_in_both_stages(self):
        a = parse_render(_render('samples=1', 'v #0:53:00 #1:53:00', 'f #0:53:00'))
        b = parse_render(_render('samples=1', 'v #0:53:01 #1:53:01', 'f #0:53:01'))
        derived = {r.key() for r in derive.neighbours([a, b])}
        self.assertIn(
            (
                a.target,
                a.layout,
                parse_stage(f'v lib={LIB} #0:53:01 #1:53:00'),
                parse_stage(f'f lib={LIB} #0:53:01'),
                (),
            ),
            derived,
        )
        self.assertNotIn(a.key(), derived)
        self.assertNotIn(b.key(), derived)

    def test_derived_render_keeps_linked_functions_and_specialized_names(self):
        a = parse_render(_render('samples=1', 'v as=selected #0:53:00', 'f') + 'fl helper as=helper_selected | ')
        b = parse_render(_render('samples=4', 'v as=selected #0:53:01', 'f') + 'fl helper as=helper_selected | ')
        for render in [*derive.targets([a, b]), *derive.neighbours([a, b])]:
            self.assertEqual(render.vertex.specialized_name, 'selected')
            self.assertEqual(render.linked, a.linked)
            self.assertEqual(parse_render(render.text()), render)


class DispatchTest(unittest.TestCase):
    def test_d_line_names_its_state_as_a_c_line(self):
        state = f'form=f opt=0 | k blur lib={LIB} #0:53:01 | '
        fields, kernel = parse_kernel(f'D ok=1 tg=32,8,1 edge=0,0,0 threads=0 | {state}')
        self.assertEqual((fields, kernel), parse_kernel(f'C ok=1 {state}'))

    def test_d_lines_are_not_taken_as_kernels_seen(self):
        index = {LIB: [Function('blur', KERNEL, ())]}
        corpus = [f'D ok=1 tg=32,8,1 edge=0,0,0 threads=0 | form=f opt=0 | k blur lib={LIB} | ']
        lines = plan.lines_for('wide', corpus, index)
        self.assertEqual(lines['corpus'], corpus)
        self.assertEqual(lines['kernels'], [f'C ok=1 form=f opt=0 | k blur lib={LIB} | '])


if __name__ == '__main__':
    unittest.main()
