#!/usr/bin/env python3
"""raw/test/inventory.tsv stays the inventory of what the kinds send and m2v implements.

selcover (raw/test/selcover.mm, over the host harness) writes the inventory and its baseline from the raw device's own
objects; this checks, with no harness, that the committed files still describe the kinds' sent tables: every selector a
kind sends has its row naming that kind and no row names a kind that does not send it, the baseline's gaps are the
inventory's, and every refusal by design names an inventory row and gives its reason. A kind table that gains a selector
fails here until selcover is run again (SELCOVER_WRITE=1).
"""

import csv
import json
import unittest
from pathlib import Path

ROOT = Path(__file__).parents[3]
TEST = ROOT / 'drivers/TegraGM20BMetal/raw/test'
KINDS = sorted((ROOT / 'drivers/NativeMetal/abi/kinds').glob('*.json'))
FIRST_PARTY = ('raw', 'zero', 'refused')


def lines(path: Path) -> list[list[str]]:
    with path.open() as f:
        return [row for row in csv.reader(f, delimiter='\t') if row and not row[0].startswith('#')]


class Inventory(unittest.TestCase):
    def setUp(self) -> None:
        self.rows = lines(TEST / 'inventory.tsv')
        self.sent = {}
        for path in KINDS:
            kind = json.loads(path.read_text())
            for protocol, selectors in kind['sent'].items():
                for selector in selectors:
                    self.sent.setdefault((protocol, selector), set()).add(kind['kind'])

    def test_rows_have_six_fields(self) -> None:
        for row in self.rows:
            self.assertEqual(len(row), 6, row)

    def test_every_sent_selector_has_its_row(self) -> None:
        named = {}
        for container, _, selector, _, sent, _ in self.rows:
            if sent != '-':
                named.setdefault((container, selector), set()).update(sent.split(','))
        for key, kinds in self.sent.items():
            self.assertEqual(named.get(key, set()), kinds, f'{key}: the kinds that send it')
        for key in named:
            self.assertIn(key, self.sent, f'{key}: no kind sends it')

    def test_baseline_gaps_are_the_inventory_rows(self) -> None:
        answers = {(c, s): raw for c, _, s, _, sent, raw in self.rows if sent != '-'}
        gaps = set()
        for kind, protocol, selector, how in (line[0].split(' ') for line in lines(TEST / 'selgaps.txt')):
            gaps.add((protocol, selector))
            self.assertIn(answers.get((protocol, selector)), (how, 'refused'), f'{kind} {protocol} {selector}')
        for (protocol, selector), raw in answers.items():
            if raw not in FIRST_PARTY:
                self.assertIn((protocol, selector), gaps, f'{protocol} {selector} is {raw}: not in selgaps.txt')

    def test_refusals_name_rows_and_reasons(self) -> None:
        refused = {(c, item) for c, item, _, _, _, raw in self.rows if raw == 'refused'}
        listed = set()
        for row in lines(TEST / 'refused.txt'):
            self.assertEqual(len(row), 3, row)
            self.assertTrue(row[2].strip(), f'{row[0]} {row[1]}: no reason')
            listed.add((row[0], row[1]))
        self.assertEqual(listed - refused, set(), 'refusals with no inventory row')


if __name__ == '__main__':
    unittest.main()
