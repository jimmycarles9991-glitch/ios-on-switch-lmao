"""The app's own known-answer program (hw_test_app/bench/bench_known.h), built here the same way.

known_test.py holds the header to these formulas and runs the words through the host's Maxwell interpreter, so the
device's self-check program is checked by the tools that check every other program.
"""

import struct

KNOWN_DATA = (0x3F800000, 0x3F000000, 0x3E800000, 0x40000000)
KNOWN_GPRS = 4


def control(delay: int) -> int:
    return delay | 7 << 5 | 7 << 8


def schedule(d0: int, d1: int, d2: int) -> int:
    return control(d0) | control(d1) << 21 | control(d2) << 42


def mov_cbuf(dst: int, bank: int, offset: int) -> int:
    return 0x4C98 << 48 | 0xF << 39 | bank << 34 | (offset // 4) << 20 | 7 << 16 | dst


EXIT = 0xE300 << 48 | 7 << 16 | 0xF
NOP = 0x50B0 << 48 | 7 << 16 | 0xF << 8

KNOWN_CODE_WORDS = (
    schedule(1, 1, 1),
    mov_cbuf(0, 1, 0),
    mov_cbuf(1, 1, 4),
    mov_cbuf(2, 1, 8),
    schedule(15, 15, 1),
    mov_cbuf(3, 1, 12),
    EXIT,
    NOP,
)
KNOWN_KERNEL_WORDS = (schedule(15, 1, 1), EXIT, NOP, NOP)
KNOWN_SPH_WORDS = (2 | 3 << 5 | 5 << 10 | 1 << 14 | 1 << 17, 0, 0, 0, 0, 1 << 31, *([0] * 12), 0xF, 0)


def words64(words: tuple[int, ...]) -> bytes:
    return struct.pack(f'<{len(words)}Q', *words)


KNOWN_CODE = words64(KNOWN_CODE_WORDS)
KNOWN_KERNEL = words64(KNOWN_KERNEL_WORDS)
KNOWN_SPH = struct.pack('<20I', *KNOWN_SPH_WORDS)
KNOWN_DATA_BYTES = struct.pack('<4I', *KNOWN_DATA)
