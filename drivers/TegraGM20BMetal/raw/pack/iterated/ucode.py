"""MAXWELL_B (0xb197) iterated-blend encoding and an assembler for it.

Field positions and enum values: third_party/mesa/src/nouveau/headers/nvidia/classes/clb197.h:1095-1189.
Helper names and the operand slots each helper fills follow Ryujinx's UcodeAssembler (MIT,
Ryujinx.Graphics.Gpu/Engine/Threed/Blender/UcodeAssembler.cs), which regenerates NVN's programs.
"""

import dataclasses
import struct
from typing import TypedDict, Unpack

TEST_CC = {'F': 0, 'T': 1, 'EQ': 2, 'NE': 3, 'LT': 4, 'LE': 5, 'GT': 6, 'GE': 7}
OPERATION = {'MMADD': 0, 'MMSUB': 1, 'MIN': 2, 'MAX': 3, 'RCP': 4, 'ADD': 5, 'SUB': 6}
OP_AC = {'SRC': 0, 'DST': 1, 'SRC_A': 2, 'DST_A': 3, 'T0': 4, 'T1': 5, 'T2': 6, 'PBR': 7}
OP_BD = {
    'ZERO': 0,
    'ONE': 1,
    'SRC': 2,
    'SRC_A': 3,
    '1-SRC_A': 4,
    'DST': 5,
    'DST_A': 6,
    '1-DST_A': 7,
    'T0': 9,
    'T1': 10,
    'T2': 11,
    'PBR': 12,
    'K': 13,
    'ZERO_PRODUCT': 14,
}
SWIZZLE = {'RGB': 0, 'GBR': 1, 'RRR': 2, 'GGG': 3, 'BBB': 4, 'R_TO_A': 5}
MASK = {'RGB': 0, 'R': 1, 'G': 2, 'B': 3}
# PASS_OUTPUT: the header calls 3 NONE; Ryujinx calls it PBR (the result only lands in PBR).
DEST = {'T0': 0, 'T1': 1, 'T2': 2, 'PBR': 3}

FIELDS = (  # name, low bit, width, enum (None: plain integer)
    ('test', 0, 3, TEST_CC),
    ('op', 3, 3, OPERATION),
    ('k', 6, 3, None),
    ('a', 9, 3, OP_AC),
    ('b', 12, 4, OP_BD),
    ('c', 16, 3, OP_AC),
    ('d', 19, 4, OP_BD),
    ('swz', 23, 3, SWIZZLE),
    ('mask', 26, 2, MASK),
    ('dest', 28, 2, DEST),
    ('set_cc', 31, 1, None),
)

M_OPTIMIZATION = 0x02F4
M_ITERATED_BLEND = 0x0DD0
M_PASS = 0x0DD4
M_POINTER = 0x0DF0
M_INSTRUCTION = 0x0DF4
M_BLEND_PER_TARGET = 0x12E4
M_SEPARATE_ALPHA = 0x133C
M_COLOR_OP, M_COLOR_SRC, M_COLOR_DST = 0x1340, 0x1344, 0x1348
M_ALPHA_OP, M_ALPHA_SRC, M_ALPHA_DST = 0x134C, 0x1350, 0x1358
M_BLEND0 = 0x1360
OGL_ADD = 0x8006
OGL = {'ZERO': 0x4000, 'ONE': 0x4001, '1-SRC_A': 0x4303}
ENABLE_RGB, ENABLE_RGBA = 1, 3  # SET_ITERATED_BLEND ENABLE bit 0, ALPHA_ENABLE bit 1


def constant_method(j: int, lane: int) -> int:
    """SET_ITERATED_BLEND_CONSTANT_RED/GREEN/BLUE(j), clb197.h:2650-2657."""
    return 0x1480 + 16 * j + 4 * lane


def half_bits(x: float) -> int:
    return struct.unpack('<H', struct.pack('<e', x))[0]


def half_value(bits: int) -> float:
    return struct.unpack('<e', struct.pack('<H', bits))[0]


@dataclasses.dataclass(frozen=True)
class Insn:
    test: str = 'T'
    op: str = 'MMADD'
    k: int = 0
    a: str = 'SRC'
    b: str = 'ZERO'
    c: str = 'SRC'
    d: str = 'ZERO'
    swz: str = 'RGB'
    mask: str = 'RGB'
    dest: str = 'PBR'
    set_cc: int = 0

    def encode(self) -> int:
        word = 0
        for name, low, width, enum in FIELDS:
            raw = getattr(self, name)
            value = raw if enum is None else enum[raw]
            if not 0 <= value < 1 << width:
                raise ValueError(f'{name}={raw!r} does not fit {width} bits')
            word |= value << low
        return word

    @staticmethod
    def decode(word: int) -> 'Insn':
        fields = {}
        for name, low, width, enum in FIELDS:
            value = word >> low & (1 << width) - 1
            fields[name] = value if enum is None else {v: n for n, v in enum.items()}[value]
        if word >> 30 & 1:
            raise ValueError('bit 30 is not a field')
        return Insn(**fields)


def parse_dest(spec: str) -> tuple[str, str, str, int]:
    """'T1', 'PBR.GBR', 'T0.RRR.CC', 'T1.R_TO_A', 'T2.R' -> (register, swizzle, mask, set_cc)."""
    reg, *mods = spec.split('.')
    swz, mask, set_cc = 'RGB', 'RGB', 0
    for m in mods:
        if m == 'CC':
            set_cc = 1
        elif m in SWIZZLE:
            swz = m
        elif m in MASK:
            mask = m
        else:
            raise ValueError(spec)
    return reg, swz, mask, set_cc


class PassOptions(TypedDict, total=False):
    cc: str
    k: int


class Asm:
    def __init__(self) -> None:
        self.code: list[Insn] = []

    def emit(  # noqa: PLR0913, PLR0917  one parameter per field of the instruction word
        self,
        op: str,
        dest: str,
        a: str = 'SRC',
        b: str = 'ZERO',
        c: str = 'SRC',
        d: str = 'ZERO',
        cc: str = 'T',
        k: int = 0,
    ) -> None:
        reg, swz, mask, set_cc = parse_dest(dest)
        insn = Insn(cc, op, k, a, b, c, d, swz, mask, reg, set_cc)
        insn.encode()
        self.code.append(insn)

    def mul(self, dest: str, a: str, b: str, **kw: Unpack[PassOptions]) -> None:
        self.emit('MMADD', dest, a, b, **kw)

    def madd(self, dest: str, a: str, b: str, c: str, **kw: Unpack[PassOptions]) -> None:
        self.emit('MMADD', dest, a, b, c, 'ONE', **kw)

    def mmadd(self, dest: str, a: str, b: str, c: str, d: str, **kw: Unpack[PassOptions]) -> None:
        self.emit('MMADD', dest, a, b, c, d, **kw)

    def mmsub(self, dest: str, a: str, b: str, c: str, d: str, **kw: Unpack[PassOptions]) -> None:
        self.emit('MMSUB', dest, a, b, c, d, **kw)

    def min(self, dest: str, a: str, b: str, **kw: Unpack[PassOptions]) -> None:
        self.emit('MIN', dest, a, b, **kw)

    def max(self, dest: str, a: str, b: str, **kw: Unpack[PassOptions]) -> None:
        self.emit('MAX', dest, a, b, **kw)

    def rcp(self, dest: str, a: str, **kw: Unpack[PassOptions]) -> None:
        self.emit('RCP', dest, a, **kw)

    def mov(self, dest: str, b: str, **kw: Unpack[PassOptions]) -> None:
        self.emit('ADD', dest, b=b, **kw)

    def add(self, dest: str, b: str, d: str, **kw: Unpack[PassOptions]) -> None:
        self.emit('ADD', dest, b=b, d=d, **kw)

    def sub(self, dest: str, b: str, d: str, **kw: Unpack[PassOptions]) -> None:
        self.emit('SUB', dest, b=b, d=d, **kw)


@dataclasses.dataclass
class Program:
    name: str
    code: list[Insn]
    consts: dict[int, tuple[int, int, int]]  # j -> (r, g, b) fp16 bit patterns
    alpha: tuple[str, ...]  # ('ff', src_coeff, dst_coeff) or ('ucode',)
    uses: frozenset[str]  # model assumptions from emu.MODEL this program depends on
    note: str = ''

    @property
    def words(self) -> list[int]:
        return [i.encode() for i in self.code]

    def methods(self) -> list[tuple[int, int]]:
        """Give the (method, value) pairs in push order; the enable goes last."""
        src, dst = (self.alpha[1], self.alpha[2]) if self.alpha[0] == 'ff' else ('ONE', 'ZERO')
        out = [
            (M_OPTIMIZATION, 0),
            (M_BLEND_PER_TARGET, 0),
            (M_SEPARATE_ALPHA, 1),
            (M_COLOR_OP, OGL_ADD),
            (M_COLOR_SRC, OGL['ONE']),
            (M_COLOR_DST, OGL['ZERO']),
            (M_ALPHA_OP, OGL_ADD),
            (M_ALPHA_SRC, OGL[src]),
            (M_ALPHA_DST, OGL[dst]),
            (M_BLEND0, 1),
        ]
        for j in sorted(self.consts):
            out += [(constant_method(j, lane), bits) for lane, bits in enumerate(self.consts[j])]
        out.append((M_POINTER, 0))
        out += [(M_INSTRUCTION, w) for w in self.words]
        out.append((M_PASS, len(self.code)))
        out.append((M_ITERATED_BLEND, ENABLE_RGBA if self.alpha[0] == 'ucode' else ENABLE_RGB))
        return out
