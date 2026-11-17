"""selectorgate.py: what a draw selecting a config binds, and a config judged by its outputs and its issued count."""

import struct
import unittest
from pathlib import Path

from tools.gpu.maxwell import gate, isa, selectorgate

FIXTURE = Path(__file__).with_name('testdata') / 'image_draw_fragment.hex'


def image() -> bytes:
    words = [int(w, 16) for w in FIXTURE.read_text().split()]
    return bytes(isa.HEADER_BYTES) + b''.join(w.to_bytes(4, 'little') for w in words)


class SelectorGateTest(unittest.TestCase):
    def test_a_config_pins_its_compared_bytes_and_the_residual_words(self):
        table = {'length': 8, 'compared': 0xF7, 'bank': 3}
        value = bytes([1, 2, 3, 0, 5, 6, 7, 8])
        self.assertEqual(
            selectorgate.pins(table, value, [0x710]),
            {(0, 0x710): 0, (3, 0): (0x00FFFFFF, 0x030201), (3, 4): 0x08070605},
        )

    def test_a_partly_pinned_word_keeps_its_seeded_padding(self):
        inputs = gate.Seeded(1, 0, {(3, 0): (0x00FFFFFF, 0x030201)}).inputs()
        free = gate.Seeded(1, 0).inputs()
        self.assertEqual(inputs.cbuf(3, 0) & 0x00FFFFFF, 0x030201)
        self.assertEqual(inputs.cbuf(3, 0) >> 24, free.cbuf(3, 0) >> 24)

    def test_a_bank_permutation_keeps_correlated_words_and_partial_pins(self):
        pins = {(1, 0x30): (0x00FFFFFF, 0x030201)}
        before = gate.Seeded(1, 0, pins).inputs(bound_cbuf=1)
        after = gate.Seeded(1, 0, pins).inputs(bound_cbuf=2, cbuf_map=(0, 2, 1))
        self.assertEqual(after.bound_cbuf, 2)
        for offset in range(0, 64, 4):
            self.assertEqual(after.cbuf(2, offset), before.cbuf(1, offset))
            self.assertEqual(after.cbuf(1, offset), before.cbuf(2, offset))
        free = gate.Seeded(1, 0).inputs()
        self.assertEqual(after.cbuf(2, 0x30) & 0x00FFFFFF, 0x030201)
        self.assertEqual(after.cbuf(2, 0x30) >> 24, free.cbuf(1, 0x30) >> 24)

    def test_cbuf_loads_and_bound_texture_fetches_follow_the_same_permutation(self):
        words = isa.words_of(image()[isa.HEADER_BYTES :])
        for i, word in enumerate(words):
            if i % 4 and isa.name(word) == 'TEX_b':
                words[i] = 0xD800000000000000 | 1 << 53 | 4 << 50 | 12 << 36 | 6 << 28 | 1 << 20 | 7 << 16 | 4
        changed = words.copy()
        for i, word in enumerate(words):
            if not i % 4:
                continue
            name = isa.name(word)
            shift = 34 if name.endswith('_cbuf') else 36 if name == 'LDC' else None
            if shift is not None:
                bank = isa.field(word, shift, 5)
                changed[i] = (word & ~(31 << shift)) | (0, 2, 1)[bank] << shift
        header = bytearray(isa.HEADER_BYTES)
        struct.pack_into('<I', header, gate.OMAP_TARGET_BYTE, 0xF)
        before = bytes(header) + struct.pack(f'<{len(words)}Q', *words)
        after = bytes(header) + struct.pack(f'<{len(changed)}Q', *changed)
        pins = {(0, 0x740): 0, (1, 0x30): (0x00FFFFFF, 0x030201)}
        self.assertEqual(gate.compare((before, 1), (after, 2), 16, 1, pins, after_cbuf_map=(0, 2, 1)), 'identical')
        self.assertTrue(gate.compare((before, 1), (after, 2), 16, 1, pins).startswith('different'))
        with self.assertRaisesRegex(ValueError, 'permutation'):
            gate.compare((before, 1), (after, 2), 1, 1, pins, after_cbuf_map=(0, 1, 1))

    def test_a_config_passes_on_identical_outputs_within_the_bound(self):
        program = image().hex()
        d = {
            'key': 'k',
            'key_hash': '0',
            'fs': program,
            'residual_words': [],
            'selectors': {
                'bank': 1,
                'length': 4,
                'compared': 0xF,
                'configs': [{'value': '01020304', 'code': program}],
            },
        }
        lines, passed = selectorgate.check_state(d, gate.compare, 2, 1, 10000)
        self.assertTrue(passed, lines)
        self.assertIn(': identical; issued a quad: generic ', lines[0])
        lines, passed = selectorgate.check_state(d, gate.compare, 2, 1, 1)
        self.assertFalse(passed)
        self.assertIn('over the bound of 1', lines[0])


if __name__ == '__main__':
    unittest.main()
