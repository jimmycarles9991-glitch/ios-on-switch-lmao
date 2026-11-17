#!/usr/bin/env python3
"""The chip facts the pack generator stamps into a pack's header are raw_chip_facts.h's, read as the number it holds."""

import os
import re
import sys
import unittest

RUNTIME = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'raw', 'runtime')
sys.path.insert(0, os.path.join(RUNTIME, '..', 'pack'))
import packfile  # noqa: E402


def assumption(name: str) -> int:
    """Give raw_pack.h PackAssumption `name`'s bit."""
    with open(os.path.join(RUNTIME, 'raw_pack.h')) as header:
        return 1 << int(re.search(rf'\b{name} = 1u << (\d+)', header.read())[1])


class ChipFactsTest(unittest.TestCase):
    def test_the_pack_holds_texs_f16_as_gpu_raw_texs_f16_proved(self):
        self.assertEqual(packfile.chip_facts(), assumption('AssumeTexsF16'))


if __name__ == '__main__':
    unittest.main()
