"""packfile.py rules_identity: the header's last 16 bytes name the rules a device-made record is made by.

They change with a varying register, the baseline or the init context, and with nothing else.
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'pack'))
import packfile


def pack(blob: list[tuple[int, int]], baseline: dict[int, int], init: list[tuple[int, int, int, int]]) -> packfile.Pack:
    return packfile.Pack([{'blob': blob}], [], [], baseline, init, bytearray(), [])


class RulesIdentityTest(unittest.TestCase):
    def test_the_header_ends_in_it(self) -> None:
        p = packfile.Pack([], [], [], {0x10: 1}, [(0, 0x20, 3, 0)], bytearray(), [])
        data, _ = packfile.pack_bytes(p)
        header = 8 + 4 * 4 + 8 * 2 * packfile.SECTIONS + 16 + 8 * packfile.SECTIONS
        self.assertEqual(data[header : header + 16], packfile.rules_identity(p))

    def test_it_follows_the_rules_alone(self) -> None:
        base = packfile.rules_identity(pack([(0x400, 1)], {0x10: 1}, [(0, 0x20, 3, 0)]))
        self.assertEqual(base, packfile.rules_identity(pack([(0x400, 9)], {0x10: 1}, [(0, 0x20, 3, 0)])))
        for other in (
            pack([(0x404, 1)], {0x10: 1}, [(0, 0x20, 3, 0)]),
            pack([(0x400, 1)], {0x10: 2}, [(0, 0x20, 3, 0)]),
            pack([(0x400, 1)], {0x10: 1}, [(0, 0x20, 4, 0)]),
        ):
            self.assertNotEqual(base, packfile.rules_identity(other))

    def test_a_relocated_or_other_subchannel_init_word_is_no_context(self) -> None:
        base = packfile.rules_identity(pack([], {}, []))
        self.assertEqual(base, packfile.rules_identity(pack([], {}, [(1, 0x20, 3, 0), (0, 0x24, 5, 1)])))


if __name__ == '__main__':
    unittest.main()
