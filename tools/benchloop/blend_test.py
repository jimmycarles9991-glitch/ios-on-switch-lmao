"""Blend tables, their formulas and their experiments, through the fake device and back."""

import tempfile
import unittest
from pathlib import Path

from tools.benchloop import blend, columns, formula, protocol, report, session

EXAMPLE = Path(__file__).with_name('testdata') / 'multiply_example.json'


class FormulaTest(unittest.TestCase):
    def test_multiply(self) -> None:
        rgb = formula.Formula('src.rgb * dst.rgb + src.rgb * (1 - dst.a) + dst.rgb * (1 - src.a)')
        a = formula.Formula('src.a + dst.a - src.a * dst.a')
        out = formula.blend(rgb, a, (0.5, 1.0, 0.0, 1.0), (0.5, 0.25, 1.0, 1.0))
        self.assertEqual(out, (0.25, 0.25, 0.0, 1.0))

    def test_only_arithmetic_is_allowed(self) -> None:
        for text in ('__import__("os")', 'src.rgb[0]', 'open(1)', 'dst.q'):
            with self.assertRaises(formula.FormulaError):
                formula.Formula(text)((0, 0, 0, 0), (0, 0, 0, 0))

    def test_clamp_and_vectors(self) -> None:
        f = formula.Formula('clamp(src.rgb * 2 - 0.5, 0, 1)')
        self.assertEqual(f((0.1, 0.5, 0.9, 1), (0, 0, 0, 0)), (0.0, 0.5, 1.0))


class TableTest(unittest.TestCase):
    def test_fields_round_trip(self) -> None:
        word = blend.instruction({'operation': 'SUB_PRODUCTS', 'operand_b': 'CONSTANT_RGB', 'pass_output': 'NONE'})
        values = blend.field_values(word)
        self.assertEqual(
            (values['operation'], values['operand_b'], values['pass_output']), ('SUB_PRODUCTS', 'CONSTANT_RGB', 'NONE')
        )
        self.assertEqual(blend.instruction(f'{word:#x}'), word)
        with self.assertRaises(blend.TableError):
            blend.instruction({'operation': 'DIVIDE'})

    def test_methods_are_ones_the_device_takes(self) -> None:
        table = blend.Table.load(EXAMPLE)
        methods, restore = table.methods(True)
        self.assertEqual(methods[0], [0x12E4, 0], 'per-target blend off: the unit takes the common block')
        self.assertIn([blend.SET_ITERATED_BLEND_PASS, 1], methods)
        self.assertIn([0x1360, 1], methods)
        self.assertEqual(methods[-1], [blend.SET_ITERATED_BLEND, 1])
        self.assertIn([blend.SET_ITERATED_BLEND, 0], restore)
        self.assertTrue(all(protocol.method_allowed(m) for m, _ in methods + restore))

    def test_column_program_colours(self) -> None:
        colours = blend.colours(8, 1)
        p = columns.program(colours)
        for x in (0, 5, 7):
            self.assertEqual(columns.run(p, x), tuple(protocol_f32(c) for c in colours[x]))


def protocol_f32(x: float) -> float:
    import struct  # noqa: PLC0415

    return struct.unpack('<f', struct.pack('<f', x))[0]


class LoopTest(unittest.TestCase):
    def test_example_table_on_the_fake_device(self) -> None:
        table = blend.Table.load(EXAMPLE)
        planned = blend.generate(table, 'bgra8unorm', 16, (True, False))
        with tempfile.TemporaryDirectory() as scratch:
            results = session.fake(Path(scratch) / 'device').run_all(planned, 'blend', Path(scratch) / 'out')
        rows = report.analysis('blend', planned, results, [EXAMPLE])['blend']
        self.assertEqual([r['status'] for r in rows], ['ok', 'ok'])
        for row in rows:
            self.assertEqual(row['checked'], 256)
            self.assertEqual(row['exact'], 256, row)


if __name__ == '__main__':
    unittest.main()
