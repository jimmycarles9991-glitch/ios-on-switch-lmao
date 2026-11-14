#!/usr/bin/env python3
"""Each built list of what a binary still takes from m2v is its pinned list.

An entry that appears fails until the code that took it is fixed or the pin names it; one that goes fails until the pin
drops it, so a pin only ever says what is left. Every pin empty: no binary that ships or builds the pack needs m2v.
usage: m2v_link_test.py <built>=<pinned>...
"""

import sys
import unittest
from pathlib import Path


def entries(path: str) -> dict[str, str]:
    lines = [line.split('\t') for line in Path(path).read_text().splitlines() if line and not line.startswith('#')]
    return {fields[0]: fields[1] if len(fields) > 1 else '' for fields in lines}


class M2vLink(unittest.TestCase):
    def test_lists_are_pinned(self) -> None:
        for pair in sys.argv[1:]:
            built_path, pinned_path = pair.split('=')
            with self.subTest(pin=Path(pinned_path).name):
                built, pinned = entries(built_path), entries(pinned_path)
                self.assertEqual(sorted(built.keys() - pinned.keys()), [], 'new entries into m2v')
                self.assertEqual(sorted(pinned.keys() - built.keys()), [], 'pinned entries that are gone: unpin them')
                moved = {k: (pinned[k], built[k]) for k in built.keys() & pinned.keys() if built[k] != pinned[k]}
                self.assertEqual(moved, {}, 'entries whose referencing units changed (pinned, built)')


if __name__ == '__main__':
    unittest.main(argv=sys.argv[:1])
