"""program.py's prologue, epilogue and descriptions."""

import unittest

from tools.benchloop import protocol
from tools.benchloop.sm53 import encode as e, operands, program
from tools.benchloop.sm53.encode import Cbuf, f32
from tools.gpu.maxwell.isa import field as bits

SPECIALS = (0x7FC12345, 0x00000001, 0xFF800000, 0x80000000, 0xDEADBEEF, 0)


class ProgramTest(unittest.TestCase):
    def test_prologue_loads_registers_in_order_from_c1(self):
        prog = program.fragment([], {20: 7, 9: 3}, (9, 20))
        moves = [d for d in operands.decode(prog.code) if d.name == 'MOV_cbuf']
        loads = [(d.writes()[0], bits(d.word, 34, 5), bits(d.word, 20, 14) * 4) for d in moves]
        self.assertEqual(loads, [(9, 1, 0), (20, 1, 4)])
        self.assertEqual([d.control.delay for d in moves], [1, program.SAFE_DELAY])
        self.assertEqual(prog.data, bytes.fromhex('03000000070000000000000000000000'))
        self.assertEqual(prog.gprs, 21)

    def test_results_pack_into_normal_floats_whatever_they_hold(self):
        for x in SPECIALS:
            prog = program.fragment([e.mov(9, Cbuf(1, 0))], {8: x}, (8, 9))
            texel = program.expected_texel(prog.code, prog.data)
            self.assertEqual(program.unpack(texel), (x, x))
            self.assertEqual(texel, program.pack(x, x))
            self.assertTrue(all(0x3F800000 <= w < 0x40000000 for w in texel), [hex(w) for w in texel])
            self.assertEqual(program.texel_words(program.texel_hex(texel)), texel)

    def test_predicates_start_as_given(self):
        body = [e.sel(8, 5, 6, 2), e.sel(9, 5, 6, 3)]
        prog = program.fragment(body, {5: 1, 6: 2}, (8, 9), preds={2: True, 3: False})
        self.assertEqual(program.unpack(program.expected_texel(prog.code, prog.data)), (1, 2))

    def test_the_outputs_are_not_results(self):
        with self.assertRaises(ValueError):
            program.fragment([], {}, (2, 8))

    def test_descriptions_pass_the_protocol(self):
        frag = program.fragment([e.fadd(8, 8, 5)], {8: f32(1.0), 5: f32(2.0)}, (8, 8))
        experiment = {
            'id': 'frag',
            'kind': 'fragment',
            'target': {'format': 'rgba32float', 'width': 8, 'height': 4},
            'program': frag.wire(),
        }
        protocol.validate_experiment(experiment)
        self.assertEqual(len(protocol.unb64(experiment['program']['sph'])), protocol.SPH_BYTES)
        comp = program.compute([e.fadd(8, 8, 5)], {8: f32(1.0), 5: f32(2.0)})
        self.assertIsNone(comp.sph)
        experiment = {
            'id': 'comp',
            'kind': 'compute',
            'target': {'format': 'r32float', 'width': 1, 'height': 1},
            'compute': comp.wire() | {'threads': [32, 1, 1], 'groups': [1, 1, 1]},
        }
        protocol.validate_experiment(experiment)
        self.assertNotIn('sph', comp.wire())
        self.assertEqual(operands.decode(comp.code)[-2].name, 'EXIT')


if __name__ == '__main__':
    unittest.main()
