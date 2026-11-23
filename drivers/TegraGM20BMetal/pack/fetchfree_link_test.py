"""fetchfree_link.py: where a colour matrix form's draws read the matrix, with or without its clamp pair."""

import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'iterated'))
import fetchfree_link

BANK = 1


def ldc(offset: int) -> int:
    """Give a 32-bit LDC of constant bank BANK at `offset`."""
    return fetchfree_link.LDC_OPCODE << 51 | 4 << 48 | BANK << 36 | offset << 20


def image(offsets: list[int]) -> bytes:
    """Give a fragment program whose instructions are LDCs at `offsets`, three to each scheduling word."""
    words = []
    for k, offset in enumerate(offsets):
        if k % 3 == 0:
            words.append(0)
        words.append(ldc(offset))
    return bytes(fetchfree_link.HEADER_BYTES) + b''.join(struct.pack('<Q', w) for w in words)


def pipe(offsets: list[int]) -> dict:
    return {
        'uniform': bytes(fetchfree_link.RANGE.pack(0, BANK, 0, 0)),
        'entries': [{'kind': 'ubo', 'stage': 'f', 'slot': 1, 'ssbo': False}],
        'fs_image': image(offsets),
    }


class MatrixUniform(unittest.TestCase):
    def test_rows_and_the_clamp_pair(self):
        stored = pipe([0, 4] + [0x60 + 4 * k for k in range(11)])
        self.assertEqual(fetchfree_link.matrix_uniform(stored, pipe([0, 4])), (1, 0x60, 11))

    def test_rows_alone_where_the_matrix_has_no_clamp(self):
        stored = pipe([0, 4] + [0xB0 + 4 * k for k in range(10)])
        self.assertEqual(fetchfree_link.matrix_uniform(stored, pipe([0, 4])), (1, 0xB0, 10))

    def test_rows_a_texture_function_reads_too_hold_every_word_only_the_storage_form_reads(self):
        rows = [0xB0 + 4 * k for k in range(10)]
        self.assertEqual(fetchfree_link.matrix_uniform(pipe(rows), pipe(rows[:2])), (1, 0xB0, 10))

    def test_rows_both_forms_read_alike_are_the_matrix_where_they_are_the_only_rows(self):
        rows = [0xB0 + 4 * k for k in range(10)]
        self.assertEqual(fetchfree_link.matrix_uniform(pipe(rows), pipe(rows)), (1, 0xB0, 10))
        others = [0x40 + 4 * k for k in range(10)]
        self.assertIsNone(fetchfree_link.matrix_uniform(pipe(rows + others), pipe(rows + others)))

    def test_words_only_the_storage_form_reads_outside_the_rows_are_no_matrix(self):
        rows = [0xB0 + 4 * k for k in range(10)]
        self.assertIsNone(fetchfree_link.matrix_uniform(pipe([*rows, 0x10]), pipe(rows[:1])))


if __name__ == '__main__':
    unittest.main()
