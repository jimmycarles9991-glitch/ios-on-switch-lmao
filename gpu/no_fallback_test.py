#!/usr/bin/env python3
"""No binary that ships or builds the pack depends on the deleted Metal-to-Vulkan fallback again.

usage: no_fallback_test.py <fallback_deps>   (gpu/BUILD.bazel's genquery: each such target of their graphs)
"""

import sys
import unittest
from pathlib import Path


class NoFallback(unittest.TestCase):
    def test_no_binary_reaches_the_fallback(self) -> None:
        found = Path(sys.argv[1]).read_text().split()
        self.assertEqual(found, [], "the fallback's targets in a shipped binary's graph")


if __name__ == '__main__':
    unittest.main(argv=sys.argv[:1])
