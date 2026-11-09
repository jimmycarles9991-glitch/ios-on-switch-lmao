"""The fuc5 opcode tables (isa.py): envydis/falcon.c's tabsi and tabm for a v5 falcon without crypto, in order.

An entry is (value, mask, length, name, operand specs, ends): it matches when the little-endian instruction ANDed
with `mask` equals `value`. Length "ol0" is 3 bytes, or 4 when byte 0's bit 0 is set (the 16-bit immediate form).
Sized entries (byte 0 below 0xc0) take their operand size from byte 0's top two bits.
"""

from __future__ import annotations

Entry = tuple[int, int, int | str, str, tuple[str, ...], bool]

_ARITH = {0: 'add', 1: 'adc', 2: 'sub', 3: 'sbb', 4: 'shl', 5: 'shr', 7: 'sar', 0xC: 'shlc', 0xD: 'shrc'}


def _sub(base: int, shift: int, ops: dict[int, tuple[str, tuple[str, ...]]], length: int) -> list[Entry]:
    """Entries told apart by a 4-bit subopcode at `shift`."""
    mask = 0xF << shift | (0x3F if base < 0xC0 else 0xFF)
    return [(base | k << shift, mask, length, name, specs, False) for k, (name, specs) in ops.items()]


SIZED: list[Entry] = [
    (0x00, 0xF0, 2, 'mov', ('r0', 'fi8s'), False),
    (0x40, 0xF0, 3, 'mov', ('r0', 'fi16s'), False),
    (0x80, 0xF0, 4, 'mov', ('r0', 'fi24s'), False),
    *[(0x10 | k, 0x3F, 3, n, ('r2', 'r1', 'i8'), False) for k, n in _ARITH.items() if k < 8],
    (0x18, 0x3F, 3, 'ld', ('r2', 'Dri'), False),
    (0x1C, 0x3F, 3, 'shlc', ('r2', 'r1', 'i8'), False),
    (0x1D, 0x3F, 3, 'shrc', ('r2', 'r1', 'i8'), False),
    (0x20, 0x3F, 2, 'st', ('Dr', 'r2'), False),
    (0x21, 0x3F, 2, 'st', ('Dspr', 'r1'), False),
    (0x24, 0x3F, 2, 'cmpu', ('r1', 'r2'), False),
    (0x25, 0x3F, 2, 'cmps', ('r1', 'r2'), False),
    (0x26, 0x3F, 2, 'cmp', ('r1', 'r2'), False),
    (0x130, 0xF3F, 'ol0', 'st', ('Dsp', 'r1'), False),
    (0x430, 0xF3E, 'ol0', 'cmpu', ('r1', 'i'), False),
    (0x530, 0xF3E, 'ol0', 'cmps', ('r1', 'is'), False),
    (0x630, 0xF3E, 'ol0', 'cmp', ('r1', 'is'), False),
    (0x32, 0x3F, 2, 'mov', ('r2', 'r1'), False),
    (0x033, 0xF3F, 4, 'braeq', ('r1', 'i8', 'sfbt'), False),
    (0x433, 0xF3F, 4, 'brane', ('r1', 'i8', 'sfbt'), False),
    (0x933, 0xF3F, 5, 'braeq', ('r1', 'i8', 'lfbt'), False),
    (0xA33, 0xF3F, 5, 'braeq', ('r1', 'i16', 'sffbt'), False),
    (0xB33, 0xF3F, 6, 'braeq', ('r1', 'i16', 'lffbt'), False),
    (0xD33, 0xF3F, 5, 'brane', ('r1', 'i8', 'lfbt'), False),
    (0xE33, 0xF3F, 5, 'brane', ('r1', 'i16', 'sffbt'), False),
    (0xF33, 0xF3F, 6, 'brane', ('r1', 'i16', 'lffbt'), False),
    (0x34, 0xF3F, 3, 'ld', ('r1', 'Dsp'), False),
    (0x35, 0x3F, 3, 'st', ('Dri', 'r2'), False),
    *[(0x36 | k << 8, 0xF3E, 'ol0', n, ('r1', 'i'), False) for k, n in _ARITH.items() if k < 4],
    *[(0x36 | k << 8, 0xF3F, 3, n, ('r1', 'i'), False) for k, n in _ARITH.items() if k >= 4],
    *[(0x38 | k << 32, 0xF0000003F, 5, n, ('r2', 'r1', 'i16'), False) for k, n in _ARITH.items() if k < 4],
    *_sub(0x39, 16, {0: ('not', ('r2', 'r1')), 1: ('neg', ('r2', 'r1')), 3: ('hswap', ('r2', 'r1'))}, 3),
    *_sub(0x3A, 16, {0: ('ld', ('r1', 'Dspr'))}, 3),
    *_sub(0x3B, 16, {k: (n, ('r1', 'r2')) for k, n in _ARITH.items()}, 3),
    *_sub(
        0x3C,
        16,
        {
            **{k: (n, ('r3', 'r1', 'r2')) for k, n in _ARITH.items()},
            8: ('ld', ('r3', 'Drr')),
            9: ('st', ('Drralt', 'r2')),
        },
        3,
    ),
    *_sub(
        0x3D,
        8,
        {
            0: ('not', ('r1',)),
            1: ('neg', ('r1',)),
            2: ('mov', ('r1',)),
            3: ('hswap', ('r1',)),
            4: ('clear', ('r1',)),
            5: ('setf', ('r1',)),
        },
        2,
    ),
    (0x3E, 0xFF, 4, 'lbra', ('llt',), True),
    (0x7E, 0xFF, 4, 'lcall', ('llt',), False),
    (0x3F, 0x3F, 2, 'ld', ('r2', 'Dr'), False),
]

_IMM8 = {0xC0: 'mulu', 0xC1: 'muls', 0xC2: 'sext', 0xC4: 'and', 0xC5: 'or', 0xC6: 'xor', 0xC8: 'xbit'}
_IMM16 = {0xE0: 'mulu', 0xE1: 'muls', 0xE4: 'and', 0xE5: 'or', 0xE6: 'xor'}
_REG2 = {0: 'mulu', 1: 'muls', 2: 'sext', 4: 'and', 5: 'or', 6: 'xor', 9: 'bset', 0xA: 'bclr', 0xB: 'btgl'}
_REG3 = {0: 'mulu', 1: 'muls', 2: 'sext', 3: 'extrs', 4: 'and', 5: 'or', 6: 'xor', 7: 'extr', 8: 'xbit', 0xC: 'div'}
_IMMREG = {0x0F0: 'mulu', 0x3F0: 'sethi', 0x4F0: 'and', 0x5F0: 'or', 0x6F0: 'xor'}

UNSIZED: list[Entry] = [
    *[(op, 0xFF, 3, n, ('r2', 'r1', 'i8s' if n == 'muls' else 'i8'), False) for op, n in _IMM8.items()],
    (0xC3, 0xFF, 3, 'extrs', ('r2', 'r1', 'bitf8'), False),
    (0xC7, 0xFF, 3, 'extr', ('r2', 'r1', 'bitf8'), False),
    (0xCB, 0xFF, 3, 'ins', ('r2', 'r1', 'bitf8'), False),
    (0xCC, 0xFF, 3, 'div', ('r2', 'r1', 'i8'), False),
    (0xCD, 0xFF, 3, 'mod', ('r2', 'r1', 'i8'), False),
    (0xCE, 0xFF, 3, 'iords', ('r2', 'Iri'), False),
    (0xCF, 0xFF, 3, 'iord', ('r2', 'Iri'), False),
    (0xD0, 0xF0, 5, 'mov', ('r0', 'fi32'), False),
    *[(op, 0xFF, 4, n, ('r2', 'r1', 'i16s' if n == 'muls' else 'i16'), False) for op, n in _IMM16.items()],
    (0xE3, 0xFF, 4, 'extrs', ('r2', 'r1', 'bitf16'), False),
    (0xE7, 0xFF, 4, 'extr', ('r2', 'r1', 'bitf16'), False),
    (0xEB, 0xFF, 4, 'ins', ('r2', 'r1', 'bitf16'), False),
    (0xEC, 0xFF, 4, 'div', ('r2', 'r1', 'i16'), False),
    (0xED, 0xFF, 4, 'mod', ('r2', 'r1', 'i16'), False),
    *[(op, 0xFFE, 'ol0', n, ('r1', 'ih' if n == 'sethi' else 'i'), False) for op, n in _IMMREG.items()],
    (0x1F0, 0xFFE, 'ol0', 'muls', ('r1', 'is'), False),
    (0x2F0, 0xFFF, 'ol0', 'sext', ('r1', 'i'), False),
    (0x9F0, 0xFFF, 'ol0', 'bset', ('r1', 'i'), False),
    (0xAF0, 0xFFF, 'ol0', 'bclr', ('r1', 'i'), False),
    (0xBF0, 0xFFF, 'ol0', 'btgl', ('r1', 'i'), False),
    (0xCF0, 0xFFF, 'ol0', 'xbit', ('r1', 'flags', 'fl'), False),
    (0x8F2, 0xFFF, 3, 'setp', ('fl', 'r1'), False),
    (0xF3, 0xFF, 3, 'call', ('fct',), False),
    (0xF4, 0x20FE, 'ol0', 'bra', ('cond', 'bt'), False),
    (0x20F4, 0x3FFE, 'ol0', 'bra', ('abt',), True),
    (0x21F4, 0x3FFF, 'ol0', 'call', ('ct',), False),
    (0x28F4, 0x3FFF, 'ol0', 'sleep', ('fl',), False),
    (0x30F4, 0x3FFE, 'ol0', 'add', ('sp', 'is'), False),
    (0x31F4, 0x3FFF, 'ol0', 'bset', ('flags', 'fl'), False),
    (0x32F4, 0x3FFF, 'ol0', 'bclr', ('flags', 'fl'), False),
    (0x33F4, 0x3FFF, 'ol0', 'btgl', ('flags', 'fl'), False),
    (0xF6, 0xFF, 3, 'iowr', ('Iri', 'r2'), False),
    (0xF7, 0xFF, 3, 'iowrs', ('Iri', 'r2'), False),
    (0x0F8, 0xFFF, 2, 'ret', (), True),
    (0x1F8, 0xFFF, 2, 'iret', (), True),
    (0x2F8, 0xFFF, 2, 'exit', (), True),
    (0x3F8, 0xFFF, 2, 'xdwait', (), False),
    (0x6F8, 0xFFF, 2, 'xdfence', (), False),
    (0x7F8, 0xFFF, 2, 'xcwait', (), False),
    (0x8F8, 0xCFF, 2, 'trap', ('strap',), False),
    (0x0F9, 0xFFF, 2, 'push', ('r1',), False),
    (0x1F9, 0xFFF, 2, 'add', ('sp', 'r1'), False),
    (0x2F9, 0xFFF, 2, 'mpush', ('r1',), False),
    (0x4F9, 0xFFF, 2, 'bra', ('r1',), True),
    (0x5F9, 0xFFF, 2, 'call', ('r1',), False),
    (0x8F9, 0xFFF, 2, 'itlb', ('r1',), False),
    (0x9F9, 0xFFF, 2, 'bset', ('flags', 'r1'), False),
    (0xAF9, 0xFFF, 2, 'bclr', ('flags', 'r1'), False),
    (0xBF9, 0xFFF, 2, 'btgl', ('flags', 'r1'), False),
    *_sub(
        0xFA,
        16,
        {
            0: ('iowr', ('Ir', 'r2')),
            1: ('iowrs', ('Ir', 'r2')),
            4: ('xcld', ('r1', 'r2')),
            5: ('xdld', ('r1', 'r2')),
            6: ('xdst', ('r1', 'r2')),
            8: ('setp', ('r2', 'r1')),
        },
        3,
    ),
    (0x0FB, 0x7FF, 2, 'mpop', ('r1',), False),
    (0x1FB, 0x7FF, 2, 'mpopret', ('r1',), True),
    (0x2FB, 0x7FF, 4, 'mpopadd', ('r1', 'i16s'), False),
    (0x3FB, 0x7FF, 4, 'mpopaddret', ('r1', 'i16s'), True),
    (0x4FB, 0x7FF, 3, 'mpopadd', ('r1', 'i8s'), False),
    (0x5FB, 0x7FF, 3, 'mpopaddret', ('r1', 'i8s'), True),
    (0x0FC, 0xFFF, 2, 'pop', ('r1',), False),
    *_sub(0xFD, 16, {k: (n, ('r1', 'r2')) for k, n in _REG2.items()}, 3),
    *_sub(
        0xFE,
        16,
        {
            0: ('mov', ('s2', 'r1')),
            1: ('mov', ('r2', 's1')),
            2: ('ptlb', ('r2', 'r1')),
            3: ('vtlb', ('r2', 'r1')),
            0xC: ('xbit', ('r2', 'flags', 'r1')),
        },
        3,
    ),
    *_sub(
        0xFF,
        16,
        {
            **{k: (n, ('r3', 'r1', 'r2')) for k, n in _REG3.items()},
            0xD: ('mod', ('r3', 'r1', 'r2')),
            0xE: ('iords', ('r3', 'Irr')),
            0xF: ('iord', ('r3', 'Irr')),
        },
        3,
    ),
]
