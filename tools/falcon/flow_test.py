"""The analysis on T210's NVENC firmware.

The H.264 app's anchors (drivers/TegraNVENC/nvidia/SOURCE) and the H.265 app's picture setup and control transfers
(drivers/TegraNVENC/nvidia/SOURCE-H265).
"""

import sys
import unittest
from pathlib import Path

from tools.falcon import image
from tools.falcon.flow import Program

FIRMWARE = Path(sys.argv[1]) if len(sys.argv) > 1 else None
H264, H265 = 0xC00, 0x1F00


def _program(entry: int) -> Program:
    if FIRMWARE is None:
        raise unittest.SkipTest('needs the firmware path')
    return Program(image.parse(FIRMWARE.read_bytes()).ucode, [entry])


def _found(program: Program, kind: str, code: int, address: int | None, value: str = '') -> bool:
    return any(
        a.kind == kind and a.code == code and a.address == address and a.value.startswith(value)
        for a in program.accesses
    )


@unittest.skipIf(FIRMWARE is None, 'needs the firmware path')
class H264AppTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.program = _program(H264)

    def test_setup_and_magic(self):
        self.assertTrue(_found(self.program, 'xdld', 0x2EF9, 0xA00, 'ext 0x0 base D[0x38c].32'))
        self.assertTrue(_found(self.program, 'ld', 0x8D9A, 0xA00))

    def test_controls(self):
        self.assertTrue(_found(self.program, 'xdld', 0x2F21, 0x540, 'ext D[0xb90].32'))
        self.assertTrue(_found(self.program, 'xdld', 0x2F52, 0x600, 'ext D[0xb94].32'))
        self.assertTrue(_found(self.program, 'xdld', 0x37D0, 0x680))

    def test_io_writes(self):
        self.assertTrue(_found(self.program, 'iowr', 0x8433, 0x1010, '(D[0x5d0].16 & 0xff)'))
        self.assertTrue(_found(self.program, 'iowr', 0x3339, 0xE0C, '(D[0x4a0].32 >> 0x7)'))


@unittest.skipIf(FIRMWARE is None, 'needs the firmware path')
class H265AppTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.program = _program(H265)

    def test_setup_is_1024_bytes_at_0xa00(self):
        for i in range(4):
            self.assertTrue(_found(self.program, 'xdld', 0x1F52 + 13 * i, 0xA00 + 0x100 * i, f'ext {0x100 * i:#x}'))
        self.assertTrue(_found(self.program, 'ld', 0x14A84, 0xA00))

    def test_controls(self):
        self.assertTrue(_found(self.program, 'xdld', 0x13B6D, 0x700, 'ext D[0xb90].32'))
        self.assertTrue(_found(self.program, 'xdld', 0x13B9E, 0x800, 'ext D[0xb94].32'))
        self.assertTrue(_found(self.program, 'xdld', 0xF9AF, 0x7C0))

    def test_quant_offset_is_copied_but_never_read(self):
        self.assertTrue(_found(self.program, 'st', 0x1474B, 0x63C, 'D[0xb98].32'))
        self.assertFalse(any(a.kind == 'ld' and a.address == 0x63C for a in self.program.accesses))


if __name__ == '__main__':
    unittest.main(argv=sys.argv[:1])
