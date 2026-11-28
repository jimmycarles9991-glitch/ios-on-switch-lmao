"""pack/resolve_srgb.h's tables against metal2vulkan's measured resolve tables (bind/color_resolve.rs).

usage: resolve_srgb_test.py <resolve_srgb.h> <color_resolve.rs>
"""

import re
import struct
import sys
import unittest
from pathlib import Path

PATHS: list[Path] = []


def array(text: str, pattern: str) -> list[int]:
    body = re.search(pattern + r'[^=]*= \{([^}]*)\}', text, re.S)
    assert body, pattern
    return [int(x, 0) for x in re.findall(r'0x[0-9a-fA-F]+|\d+', re.sub(r'u\b', '', body.group(1)))]


def rust_array(text: str, name: str) -> list[int]:
    body = re.search(rf'pub const {name}: \[u16; \d+\] = \[([^\]]*)\]', text, re.S)
    assert body, name
    return [int(x) for x in re.findall(r'\d+', body.group(1))]


def f32(x: float) -> float:
    return struct.unpack('<f', struct.pack('<f', x))[0]


class ResolveSrgbTables(unittest.TestCase):
    def setUp(self) -> None:
        self.header = PATHS[0].read_text()
        self.measured = PATHS[1].read_text()

    def test_linear_values_are_metals(self) -> None:
        self.assertEqual(array(self.header, r'kSrgbLinear\[256\]'), rust_array(self.measured, 'SRGB_RESOLVE_LINEAR'))

    def test_thresholds_are_metals(self) -> None:
        self.assertEqual(
            array(self.header, r'kSrgbThresholds\[255\]'), rust_array(self.measured, 'SRGB_RESOLVE_THRESHOLDS')
        )

    def test_a_unorm8_read_gives_its_code_back(self) -> None:
        for code in range(256):
            read = f32(code / 255)
            self.assertEqual(round(f32(read * 255.0)), code)


if __name__ == '__main__':
    PATHS[:] = [Path(p) for p in sys.argv[1:3]]
    unittest.main(argv=sys.argv[:1])
