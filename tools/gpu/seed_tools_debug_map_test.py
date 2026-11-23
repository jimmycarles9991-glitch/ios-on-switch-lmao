"""The shader tools we link carry no debug map.

A debug map (the N_OSO and other stab symbols) names each archive by its path in the link's sandbox or remote worker
slot, so every relink of an unchanged tool made other bytes, and every native and raw pack bucket keyed on that tool ran
again with the same output.
"""

import os
import unittest
from pathlib import Path

from tools.common import macho

LINKED = [
    'raw/directpack',
    'raw/native-seed',
]


class DebugMap(unittest.TestCase):
    def test_no_tool_names_where_it_was_linked(self):
        tools = Path(os.environ['SEED_TOOLS'])
        for name in LINKED:
            with self.subTest(tool=name):
                data = (tools / name).read_bytes()
                stabs = macho.symbols(data, macho.macho(data), lambda kind: bool(kind & macho.N_STAB))
                self.assertEqual([s.name for s in stabs][:3], [], f'{name}: {len(stabs)} debug map symbols')
                self.assertTrue(macho.symbols(data, macho.macho(data), macho.defined_external), 'symbols stay')


if __name__ == '__main__':
    unittest.main()
