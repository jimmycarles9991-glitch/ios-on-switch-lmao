#!/usr/bin/env python3
"""SwitchGM20BMacroSlots.h's patch macro reads the root table's address where the context init writes it."""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).parents[3]
SLOTS = ROOT / 'kits/SwitchGM20BEmit/SwitchGM20BMacroSlots.h'
CONTEXT = ROOT / 'kits/SwitchGM20BEmit/context/context.tsv'


def header_scratch() -> dict[str, int]:
    """Give each scratch word the header numbers explicitly its value."""
    return {name: int(value) for name, value in re.findall(r'(SWITCH_GM20B_SCRATCH_\w+) = (\d+),', SLOTS.read_text())}


def context_root_words() -> list[int]:
    """Give the scratch words context.tsv relocates to the root table's address, high then low."""
    rows = [line.split('\t') for line in CONTEXT.read_text().splitlines() if line and not line.startswith('#')]
    by_reloc = {cols[3]: cols[1] for cols in rows if len(cols) == 5}
    return [int(re.fullmatch(r'SET_MME_SHADOW_SCRATCH\((\d+)\)', by_reloc[r]).group(1)) for r in ('root_hi', 'root_lo')]


class ScratchWords(unittest.TestCase):
    def test_the_context_init_writes_the_root_where_the_patch_macro_reads_it(self) -> None:
        root = header_scratch()['SWITCH_GM20B_SCRATCH_ROOT_HI']
        self.assertEqual(context_root_words(), [root, root + 1])


if __name__ == '__main__':
    unittest.main()
