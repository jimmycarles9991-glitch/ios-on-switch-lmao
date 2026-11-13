"""The app's known-answer program and EXIT kernel are what known.py builds, and run right on the host interpreter."""

import struct
import unittest
from pathlib import Path

from tools.benchloop import known
from tools.gpu.maxwell import isa
from tools.gpu.maxwell.execute import run
from tools.gpu.maxwell.interp import Inputs

HEADER = Path(__file__).resolve().parents[2] / 'hw_test_app' / 'bench' / 'bench_known.h'


class KnownTest(unittest.TestCase):
    def test_header_builds_the_same_words(self) -> None:
        text = ''.join(HEADER.read_text().split())
        for line in (
            'return delay | 7u << 5 | 7u << 8;',
            'return control(d0) | control(d1) << 21 | control(d2) << 42;',
            (
                'return 0x4c98ull << 48 | 0xfull << 39 | uint64_t(bank) << 34 | uint64_t(offset / 4) << 20 '
                '| 7ull << 16 | dst;'
            ),
            'kExit = 0xe300ull << 48 | 7ull << 16 | 0xf;',
            'kNop = 0x50b0ull << 48 | 7ull << 16 | 0xfull << 8;',
            (
                'schedule(1, 1, 1), movCbuf(0, 1, 0), movCbuf(1, 1, 4), movCbuf(2, 1, 8), schedule(15, 15, 1), '
                'movCbuf(3, 1, 12), kExit, kNop,'
            ),
            'kKnownKernel[] = {schedule(15, 1, 1), kExit, kNop, kNop};',
            'kPositionW = 1u << 31;',
            (
                '{2 | 3 << 5 | 5 << 10 | 1 << 14 | 1 << 17, 0, 0, 0, 0, kPositionW, '
                '0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xf, 0};'
            ),
            'kKnownData[4] = {0x3f800000, 0x3f000000, 0x3e800000, 0x40000000};',
        ):
            self.assertIn(''.join(line.split()), text)

    def test_instructions_decode_as_meant(self) -> None:
        names = [isa.name(w) for w in isa.instructions(known.KNOWN_CODE)]
        self.assertEqual(names, ['MOV_cbuf'] * 4 + ['EXIT', 'NOP'])
        self.assertEqual([isa.name(w) for w in isa.instructions(known.KNOWN_KERNEL)], ['EXIT', 'NOP', 'NOP'])

    def test_program_writes_its_shader_data(self) -> None:
        data = known.KNOWN_DATA
        inputs = Inputs(
            attribute=lambda _: 0,
            cbuf=lambda bank, offset: data[offset // 4] if bank == 1 else 0,
            texture=lambda _: [0, 0, 0, 0],
        )
        regs, killed = run(known.KNOWN_CODE, inputs, 4)
        self.assertFalse(killed)
        self.assertEqual(tuple(regs), data)

    def test_header_marks_a_pixel_program_writing_colour_zero(self) -> None:
        words = struct.unpack('<20I', known.KNOWN_SPH)
        self.assertEqual(words[0] & 0x1F, 2)
        self.assertEqual(words[0] >> 10 & 0xF, 5)
        self.assertEqual(words[18], 0xF)
        self.assertEqual(words[5], 1 << 31, 'position W: the GPU traps a pixel header without it')


if __name__ == '__main__':
    unittest.main()
