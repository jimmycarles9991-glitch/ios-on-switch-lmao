"""Every iterated blend program the pack holds against QuartzCore's formula, and the record packfile.py lays out."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import emu
import microcode
import table
from reference import apple
from ucode import ENABLE_RGB, ENABLE_RGBA, OGL


class Programs(unittest.TestCase):
    def test_each_matches_quartzcore_in_exact_arithmetic(self):
        options = {prog.name: opts for prog, opts in microcode.build()}
        for name, prog in table.PROGRAMS.items():
            with self.subTest(name):
                params, tol = options[name].get('params'), options[name].get('tol', 1e-6)
                ok, exact, _fp16, worst = emu.check(
                    prog, lambda src, dst, name=name, params=params: apple(name, src, dst, params), count=300, tol=tol
                )
                self.assertTrue(ok, f'{name}: {exact} off at {worst}')

    def test_each_fits_the_program_ram_and_ends_in_temp0(self):
        for name, prog in table.PROGRAMS.items():
            with self.subTest(name):
                self.assertLessEqual(len(prog.words), 128)
                self.assertEqual(prog.code[-1].dest, 'T0')
                self.assertLessEqual(len(prog.consts), 8)


class Names(unittest.TestCase):
    def test_a_range_flag_picks_its_form_where_one_is_written(self):
        self.assertEqual(table.program_name('cdg', True, False), 'cdg.x')
        self.assertEqual(table.program_name('cdg', False, True), 'cdg.u')
        self.assertEqual(table.program_name('mpy', False, False), 'mpy')
        self.assertIsNone(table.program_name('mpy', True, False))
        self.assertIsNone(table.program_name('vcm', False, False))
        self.assertIsNone(table.program_name('cdg', True, True))


class Record(unittest.TestCase):
    def test_alpha_from_the_common_block_or_the_program(self):
        mpy, sds = table.record(table.PROGRAMS['mpy']), table.record(table.PROGRAMS['sds'])
        self.assertEqual(
            (mpy['enable'], mpy['alpha_source'], mpy['alpha_dest']), (ENABLE_RGB, OGL['ONE'], OGL['1-SRC_A'])
        )
        self.assertEqual(sds['enable'], ENABLE_RGBA)
        self.assertEqual(mpy['words'], table.PROGRAMS['mpy'].words)


if __name__ == '__main__':
    unittest.main()
