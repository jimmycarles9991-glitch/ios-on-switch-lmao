#!/usr/bin/env python3
"""SwitchGM20BMacroSlots.h's NVK slots and scratch words are nvk_mme.h's.

NVK loads its macros in its enum's order, slot by slot, and keeps its scratch words in enum nvk_mme_scratch's order.
The kernel's push profile reads NVK's clear and anti-alias calls and both drivers' draws by these numbers, the raw
path's draw macros take NVK's draw macros' slots, its scratch words leave FECS's mailbox alone, and its patch macro
reads the root table's address where the context init writes it for NVK's SELECT_CB0: a Mesa update that moves NVK's
enums fails here first.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).parents[3]
SLOTS = ROOT / 'kits/SwitchGM20BEmit/SwitchGM20BMacroSlots.h'
NVK = ROOT / 'third_party/mesa/src/nouveau/vulkan/nvk_mme.h'
CONTEXT = ROOT / 'kits/SwitchGM20BEmit/context/context.tsv'


def nvk_slots() -> dict[str, int]:
    """Give each NVK_MME_* macro its slot, its place in enum nvk_mme."""
    body = re.search(r'enum nvk_mme \{(.*?)\};', NVK.read_text(), re.DOTALL).group(1)
    names = re.findall(r'^\s*(NVK_MME_\w+),', body, re.MULTILINE)
    return {name: slot for slot, name in enumerate(names)}


def nvk_scratch() -> dict[str, int]:
    """Give each NVK_MME_SCRATCH_* word its index, up to the first whose value is an expression of others."""
    body = re.search(r'enum nvk_mme_scratch \{(.*?)\};', NVK.read_text(), re.DOTALL).group(1)
    body = re.sub(r'/\*.*?\*/', '', body, flags=re.DOTALL)
    words, index = {}, 0
    for name, value in re.findall(r'^\s*(NVK_MME_SCRATCH_\w+)\s*(?:=\s*([^,]+))?,', body, re.MULTILINE):
        if value:
            if not value.strip().isdigit():
                break
            index = int(value)
        words[name] = index
        index += 1
    return words


def header_scratch() -> dict[str, tuple[int, str, int]]:
    """Give each scratch word the header numbers explicitly its value, the NVK word its comment names and the offset."""
    found = {}
    for name, value, nvk, plus in re.findall(
        r'(SWITCH_GM20B_SCRATCH_\w+) = (\d+),\s*// (NVK_MME_SCRATCH_\w+)(?: \+ (\d+))?', SLOTS.read_text()
    ):
        found[name] = (int(value), nvk, int(plus or 0))
    return found


def context_root_words() -> list[int]:
    """Give the scratch words context.tsv relocates to the root table's address, high then low."""
    rows = [line.split('\t') for line in CONTEXT.read_text().splitlines() if line and not line.startswith('#')]
    by_reloc = {cols[3]: cols[1] for cols in rows if len(cols) == 5}
    return [int(re.fullmatch(r'SET_MME_SHADOW_SCRATCH\((\d+)\)', by_reloc[r]).group(1)) for r in ('root_hi', 'root_lo')]


def header_slots() -> dict[str, tuple[int, str]]:
    """Give each slot the header numbers explicitly its value and the NVK macro its comment names."""
    found = {}
    for name, value, nvk in re.findall(r'(SWITCH_GM20B_MACRO_\w+) = (\d+),\s*// (NVK_MME_\w+)', SLOTS.read_text()):
        found[name] = (int(value), nvk)
    return found


class MacroSlots(unittest.TestCase):
    def test_each_nvk_slot_is_nvk_mmes(self) -> None:
        nvk = nvk_slots()
        named = header_slots()
        self.assertEqual(
            set(named),
            {
                'SWITCH_GM20B_MACRO_NVK_CLEAR',
                'SWITCH_GM20B_MACRO_NVK_SET_ANTI_ALIAS',
                'SWITCH_GM20B_MACRO_DRAWS_FIRST',
                'SWITCH_GM20B_MACRO_DRAWS_LAST',
            },
        )
        for name, (value, macro) in named.items():
            self.assertEqual(nvk[macro], value, f'{name} names {macro}, which NVK loads at slot {nvk[macro]}')

    def test_nvks_draws_are_the_range_the_profile_counts(self) -> None:
        nvk = nvk_slots()
        draws = [slot for name, slot in nvk.items() if name.startswith('NVK_MME_DRAW') and 'MESH' not in name]
        named = header_slots()
        self.assertEqual(min(draws), named['SWITCH_GM20B_MACRO_DRAWS_FIRST'][0])
        self.assertEqual(max(draws), named['SWITCH_GM20B_MACRO_DRAWS_LAST'][0])
        self.assertEqual(len(draws), max(draws) - min(draws) + 1)


class ScratchWords(unittest.TestCase):
    def test_each_nvk_word_is_nvk_mmes(self) -> None:
        nvk = nvk_scratch()
        named = header_scratch()
        self.assertEqual(set(named), {'SWITCH_GM20B_SCRATCH_FECS_END', 'SWITCH_GM20B_SCRATCH_ROOT_HI'})
        for name, (value, word, plus) in named.items():
            self.assertEqual(nvk[word] + plus, value, f"{name} names {word}, NVK's word {nvk[word]}")

    def test_fecs_mailbox_is_nvks_falcon_words(self) -> None:
        nvk = nvk_scratch()
        falcon = sorted(i for name, i in nvk.items() if name.startswith('NVK_MME_SCRATCH_FALCON_'))
        self.assertEqual(falcon, list(range(header_scratch()['SWITCH_GM20B_SCRATCH_FECS_END'][0])))

    def test_the_context_init_writes_the_root_where_the_patch_macro_reads_it(self) -> None:
        root = header_scratch()['SWITCH_GM20B_SCRATCH_ROOT_HI'][0]
        self.assertEqual(context_root_words(), [root, root + 1])


if __name__ == '__main__':
    unittest.main()
