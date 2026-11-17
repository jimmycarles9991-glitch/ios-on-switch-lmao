"""Every iterated blend program the pack or the colour matrix macro loads, under the chip's rules (ibprogram.py)."""

import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'drivers/TegraGM20BMetal/raw/pack/iterated'))

import microcode
import programs_long
import table
from ucode import M_INSTRUCTION, M_ITERATED_BLEND, M_PASS, M_POINTER

from tools.benchloop import ibprogram

MACROS = Path(__file__).resolve().parents[2] / 'kits/SwitchGM20BEmit/mme/switch_gm20b_macros.c'


def macro_arrays() -> dict[str, list[int]]:
    text = MACROS.read_text()
    found = re.finditer(r'static const uint32_t (vcm_\w+)\[\] = \{([^}]*)\}', text)
    return {m[1]: [int(w, 16) for w in re.findall(r'0x[0-9a-fA-F]+', m[2])] for m in found}


def macro_program(clamp: str) -> list[int]:
    """Give the words SWITCH_GM20B_MACRO_VCM_CONSTANTS loads for a clamp pair's form."""
    a = macro_arrays()
    middle = [] if clamp == 'none' else a['vcm_clamp_head'] + a['vcm_' + clamp]
    return a['vcm_head'] + middle + a['vcm_tail']


def loaded(words: list[int], enable: int) -> list[list[int]]:
    return [[M_POINTER, 0], *[[M_INSTRUCTION, w] for w in words], [M_PASS, len(words)], [M_ITERATED_BLEND, enable]]


class Rules(unittest.TestCase):
    def assert_runs(self, name: str, methods: list) -> None:
        methods = [list(m) for m in methods]
        self.assertIsNone(ibprogram.refusal(methods), name)
        last = [v for m, v in methods if m == M_INSTRUCTION][-1]
        self.assertNotEqual(last >> 23 & 7, ibprogram.R_TO_A, f'{name}: R_TO_A as the last pass leaves alpha wrong')

    def test_every_program_of_the_pack_and_the_bench_runs(self):
        for prog, _ in microcode.build():
            with self.subTest(prog.name):
                self.assert_runs(prog.name, prog.methods())
        for name, prog in table.PROGRAMS.items():
            with self.subTest(name):
                self.assert_runs(name, prog.methods())

    def test_the_colour_matrix_macro_loads_the_program_it_is_written_from(self):
        for (p3, p4), clamp in zip(microcode.VCM_CLAMPS, ('none', 'clip', 'scale'), strict=True):
            with self.subTest(clamp):
                words = macro_program(clamp)
                self.assertEqual(words, programs_long.vcm(microcode.IDENTITY, p3, p4).words)
                self.assert_runs(clamp, loaded(words, 3))

    def test_the_rules_refuse_the_words_the_chip_refused(self):
        # boot 21's DATA_ERROR INVALID_VALUE at 0x0df4 (the macro's first word before its constants were renumbered),
        # and the HSL programs' MAX(alpha, constant 1)
        for word in (0x3200D799, 0x3000D659, 0x3000D459):
            self.assertIsNotNone(ibprogram.refusal(loaded([word, 0x9029], 1)), hex(word))


if __name__ == '__main__':
    unittest.main()
