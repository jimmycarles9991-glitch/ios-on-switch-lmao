"""sph.py's pixel header read back through cla097sph.h's NVA097_SPHV3T2 field macros."""

import re
import unittest
from pathlib import Path

from tools.benchloop.sm53 import sph

HEADER = Path(__file__).parents[3] / 'third_party/mesa/src/nouveau/headers/nvidia/classes/cla097sph.h'
PREFIX = 'NVA097_SPHV3T2_'
FIELD = re.compile(rf'#define {PREFIX}(\w+)\s+MW\((\d+):(\d+)\)')
INDEXED = re.compile(rf'#define {PREFIX}(\w+)\(i\)\s+MW\(\((\d+)\+\(i\)\*(\d+)\):\((\d+)\+\(i\)\*\d+\)\)')
VALUE = re.compile(rf'#define {PREFIX}(\w+)\s+(0x[0-9A-Fa-f]+)\b')


class Fields:
    def __init__(self, text: str) -> None:
        self.plain = {m[1]: (int(m[3]), int(m[2])) for m in FIELD.finditer(text)}
        self.indexed = {m[1]: (int(m[4]), int(m[2]), int(m[3])) for m in INDEXED.finditer(text)}
        self.values = {m[1]: int(m[2], 16) for m in VALUE.finditer(text)}

    def read(self, header: bytes, name: str, i: int | None = None) -> int:
        value = int.from_bytes(header, 'little')
        if i is None:
            lo, hi = self.plain[name]
        else:
            lo0, hi0, stride = self.indexed[name]
            lo, hi = lo0 + i * stride, hi0 + i * stride
        return value >> lo & ((1 << (hi - lo + 1)) - 1)


class SphTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.f = Fields(HEADER.read_text())

    def test_common_fields(self):
        h = sph.pixel()
        f = self.f
        self.assertEqual(len(h), 80)
        self.assertEqual(f.read(h, 'SPH_TYPE'), f.values['SPH_TYPE_TYPE_02_PS'])
        self.assertEqual(f.read(h, 'VERSION'), 3)
        self.assertEqual(f.read(h, 'SHADER_TYPE'), f.values['SHADER_TYPE_PIXEL'])
        self.assertEqual(f.read(h, 'MRT_ENABLE'), f.values['MRT_ENABLE_TRUE'])
        self.assertEqual(f.read(h, 'KILLS_PIXELS'), f.values['KILLS_PIXELS_FALSE'])
        self.assertEqual(f.read(h, 'SASS_VERSION'), 1)
        self.assertEqual(f.read(sph.pixel(kills=True), 'KILLS_PIXELS'), f.values['KILLS_PIXELS_TRUE'])

    def test_output_map(self):
        h = sph.pixel(targets=0xF | 0x3 << 4)
        f = self.f
        for i, want in ((0, (1, 1, 1, 1)), (1, (1, 1, 0, 0)), (2, (0, 0, 0, 0))):
            got = tuple(f.read(h, c, i) for c in ('OMAP_RED', 'OMAP_GREEN', 'OMAP_BLUE', 'OMAP_ALPHA'))
            self.assertEqual(got, want, i)
        self.assertEqual(f.read(h, 'OMAP_SAMPLE_MASK'), 0)
        self.assertEqual(f.read(h, 'OMAP_DEPTH'), 0)

    def test_input_maps(self):
        h = sph.pixel({0x70: 0, 0x74: 0, 0x80: 1, 0x94: 2})
        f = self.f
        self.assertEqual(f.read(h, 'IMAP_POSITION_X'), 1)
        self.assertEqual(f.read(h, 'IMAP_POSITION_Y'), 1)
        self.assertEqual(f.read(h, 'IMAP_POSITION_Z'), 0)
        self.assertEqual(f.read(h, 'GENERIC_IMAP_X', 0), f.values['GENERIC_IMAP_X_PERSPECTIVE'])
        self.assertEqual(f.read(h, 'GENERIC_IMAP_Y', 1), f.values['GENERIC_IMAP_Y_CONSTANT'])
        self.assertEqual(f.read(h, 'GENERIC_IMAP_X', 1), f.values['GENERIC_IMAP_X_UNUSED'])
        self.assertEqual(f.read(sph.pixel(), 'IMAP_POSITION_X'), 0)

    def test_position_w_is_always_read(self):
        # NAK's from_nir: "Required on fragment shaders, otherwise it cause a trap" (the device's gr SHADER a0000800)
        for h in (sph.pixel(), sph.pixel({0x80: 1}), sph.pixel({0x7C: 0})):
            self.assertEqual(self.f.read(h, 'IMAP_POSITION_W'), 1)

    def test_the_ticket_bit_is_clcb97s(self):
        text = (HEADER.parent / 'clcb97sph.h').read_text()
        bit = int(re.search(r'NVCB97_SPHV4T2_READS_INTERLOCK_TICKET\s+MW\((\d+):', text)[1])
        self.assertEqual(sph.READS_INTERLOCK_TICKET, bit)
        set_bit = int.from_bytes(sph.pixel(reads_ticket=True), 'little') ^ int.from_bytes(sph.pixel(), 'little')
        self.assertEqual(set_bit, 1 << bit)


if __name__ == '__main__':
    unittest.main()
