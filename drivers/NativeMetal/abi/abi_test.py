"""The readers' decoding on known values, and every committed kind's table against itself and its header."""

import json
import re
import unittest
from pathlib import Path

from drivers.NativeMetal.abi.dyld_cache import decode_pointer
from drivers.NativeMetal.abi.extract import closure, struct_fields
from drivers.NativeMetal.abi.gen import header

HERE = Path(__file__).resolve().parent
KINDS = sorted((HERE / 'kinds').glob('*.json'))


class Decoding(unittest.TestCase):
    def test_struct_fields_take_natural_alignment(self):
        fields, size = struct_fields('{?="a"I"b"f"c"I"d"Q"e"B}')
        self.assertEqual(fields, [('a', 'I', 0), ('b', 'f', 4), ('c', 'I', 8), ('d', 'Q', 16), ('e', 'B', 24)])
        self.assertEqual(size, 32)

    def test_slide_v5_pointers_are_offsets_from_the_base(self):
        base = 0x180000000
        self.assertEqual(decode_pointer((1 << 63) | (0xABC << 34) | 0x25CFB2C0, 5, (base, 0)), base + 0x25CFB2C0)
        self.assertEqual(decode_pointer((0x12 << 34) | 0x1234, 5, (base, 0)), base + 0x1234)

    def test_slide_v3_plain_pointers_keep_their_address_and_auth_ones_are_offsets(self):
        self.assertEqual(decode_pointer((5 << 51) | 0x1EE050B38, 3, (0x180000000, 0)), 0x1EE050B38)
        self.assertEqual(decode_pointer((1 << 63) | 0x75343318, 3, (0x180000000, 0)), 0x1F5343318)

    def test_slide_v2_pointers_lose_their_delta(self):
        self.assertEqual(decode_pointer((0x40 << 40) | 0x1C0001234, 2, (0x00FFFF0000000000, 0)), 0x1C0001234)
        self.assertEqual(decode_pointer(0x0001000000000000, 2, (0x00FFFF0000000000, 0)), 0)

    def test_closure_follows_adopted_protocols_and_leaves_nsobject_out(self):
        found = {
            'P': {'protocols': ['Q', 'NSObject'], 'methods': {'p': 'v16@0:8'}},
            'Q': {'protocols': [], 'methods': {'q': 'B16@0:8'}},
            'NSObject': {'protocols': [], 'methods': {'hash': 'Q16@0:8'}},
        }
        self.assertEqual(closure(found, ['P']), {'p': 'v16@0:8', 'q': 'B16@0:8'})


class Tables(unittest.TestCase):
    def test_each_kind_lays_its_limits_inside_the_device(self):
        self.assertGreaterEqual(len(KINDS), 2)
        for path in KINDS:
            device = json.loads(path.read_text())['MTLDevice']
            self.assertLessEqual(device['limits_ivar'] + device['limits_bytes'], device['instance_size'], path.stem)
            for name, offset in device['limits'].items():
                self.assertEqual(device['limits_fields'][name][0], offset, f'{path.stem}: {name}')
                self.assertIn(device['limits_fields'][name][1], ('I', 'Q'), f'{path.stem}: {name}')

    def test_each_kinds_header_holds_its_size_and_limits_in_order(self):
        for path in KINDS:
            table = json.loads(path.read_text())
            text = header(table)
            fields = table['MTLDevice']['limits_fields']
            written = re.findall(r'\{"(\w+)", (0x[0-9a-f]+), (\d)\}', text)
            self.assertEqual(
                [(name, int(offset, 16), int(width)) for name, offset, width in written],
                [
                    (name, offset, 8 if fields[name][1] == 'Q' else 4)
                    for name, offset in table['MTLDevice']['limits'].items()
                ],
                path.stem,
            )
            self.assertIn(f'kMTLDeviceInstanceSize = {table["MTLDevice"]["instance_size"]};', text)
            self.assertIn(f'kMTLDeviceLimitsBytes = {table["MTLDevice"]["limits_bytes"]};', text)
            self.assertIn(f'#define SWITCH_METAL_COMPILER "{table["compiler"]}"', text)


if __name__ == '__main__':
    unittest.main()
