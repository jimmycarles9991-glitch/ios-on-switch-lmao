"""One fragment invocation of Maxwell machine code, run on the host: the gate a codegen change passes.

Field layouts are those NAK's sm50.rs encoder writes; float arithmetic is floats.py's exact IEEE. The environment is
deterministic and explicit: attributes, constant buffers and texture results come from `Inputs`, so two programs NAK
made of one shader can be run on the same inputs and their outputs compared bit for bit. A texture result is a pure
function of the instruction's handle, coordinates and level operands; MUFU is the correctly rounded function. Both are
models, the same for every program, never the hardware's filtering or approximation.

This is one lane; quad.py runs four in lockstep for the instructions that read other lanes (SHFL, FSWZADD, VOTE).
"""

import math
import struct
from collections.abc import Callable
from dataclasses import dataclass, field

from tools.gpu.maxwell.floats import F16, F32, arith, convert, flush, is_nan
from tools.gpu.maxwell.isa import field as bits

RZ_REG = 255
PT = 7
MAX_STEPS = 1_000_000


class UnsupportedError(Exception):
    pass


@dataclass
class Inputs:
    attribute: Callable[[int], int]  # IPA's attribute word at a byte address
    cbuf: Callable[[int, int], int]  # a 32-bit word of bank, byte offset
    texture: Callable[[tuple], list[int]]  # four result words of a fetch's canonical key (texture.py)
    bound_cbuf: int = -1  # the constant buffer bound textures read handles from (SET_BINDLESS_TEXTURE); -1: none
    pixel: Callable[[int], int] = lambda _mode: 0  # PIXLD's word of a mode (5: MyIndex, the invocation's sample)


@dataclass
class Lane:
    regs: list[int] = field(default_factory=lambda: [0] * 256)
    preds: list[bool] = field(default_factory=lambda: [False] * 7 + [True])
    killed: bool = False
    steps: int = 0
    stores: list = field(default_factory=list)
    local: dict = field(default_factory=dict)  # LDL and STL's memory, a word per byte address
    carry: bool = False  # the CC carry an IADD.CC wrote, which IADD.X adds

    def r(self, i: int) -> int:
        return 0 if i == RZ_REG else self.regs[i]

    def w(self, i: int, v: int) -> None:
        if i != RZ_REG:
            self.regs[i] = v & 0xFFFFFFFF

    def p(self, i: int, neg: bool = False) -> bool:
        return self.preds[i] != neg

    def setp(self, i: int, v: bool) -> None:
        if i != PT:
            self.preds[i] = v


def f20(word: int) -> int:
    return (bits(word, 20, 19) << 12) | (bits(word, 56, 1) << 31)


def i20(word: int) -> int:
    v = bits(word, 20, 19) | (bits(word, 56, 1) << 19)
    return (v - (1 << 20)) & 0xFFFFFFFF if v & (1 << 19) else v


def fmod(v: int, absolute: bool, neg: bool, width: int = 32) -> int:
    sign = 1 << (width - 1)
    if absolute:
        v &= sign - 1
    return v ^ sign if neg else v


def operand_b(lane: Lane, word: int, name: str, inputs: Inputs, imm: Callable[[int], int]) -> int:
    form = name.rsplit('_', 1)[-1]
    if form == 'reg':
        return lane.r(bits(word, 20, 8))
    if form == 'cbuf':
        return inputs.cbuf(bits(word, 34, 5), bits(word, 20, 14) * 4)
    if form == 'imm':
        return imm(word)
    raise UnsupportedError(name)


def to_f32(x: float) -> int:
    if math.isnan(x):
        return F32.nan
    try:
        return struct.unpack('<I', struct.pack('<f', x))[0]
    except OverflowError:
        return F32.inf | (F32.sign_bit if x < 0 else 0)


def from_f32(b: int) -> float:
    return struct.unpack('<f', struct.pack('<I', b))[0]


def halves(v: int, swizzle: int) -> tuple[int, int]:
    lo, hi = v & 0xFFFF, v >> 16
    if swizzle == 0:
        return lo, hi
    if swizzle == 2:
        return lo, lo
    if swizzle == 3:
        return hi, hi
    raise UnsupportedError('HADD2/HMUL2/HFMA2 with an F32 operand')


def half_op(word: int, name: str, lane: Lane) -> int:
    a = halves(lane.r(bits(word, 8, 8)), bits(word, 47, 2))
    b = halves(lane.r(bits(word, 20, 8)), bits(word, 28, 2))
    base = name.split('_', 1)[0]
    ftz = bool(bits(word, 39, 1)) if base == 'HADD2' else False
    out = []
    for k in range(2):
        x, y = a[k], b[k]
        if base == 'HADD2':
            x = fmod(x, bits(word, 44, 1), bits(word, 43, 1), 16)
            y = fmod(y, bits(word, 30, 1), bits(word, 31, 1), 16)
            r = arith('add', [x, y], F16, ftz=ftz)
        elif base == 'HMUL2':
            x = fmod(x, bits(word, 44, 1), bits(word, 31, 1), 16)
            y = fmod(y, bits(word, 30, 1), False, 16)
            r = arith('mul', [x, y], F16)
        else:
            c = halves(lane.r(bits(word, 39, 8)), bits(word, 35, 2))[k]
            x = fmod(x, False, bits(word, 31, 1), 16)
            r = arith('fma', [x, y, fmod(c, False, bits(word, 30, 1), 16)], F16)
        if bits(word, 32, 1):
            r = saturate(r, F16)
        out.append(r)
    if bits(word, 49, 2) == 1:
        return convert(flush(out[0], F16), F16, F32)
    return out[0] | (out[1] << 16)


def saturate(v: int, f: object) -> int:
    if is_nan(v, f) or v & f.sign_bit:
        return 0
    one = f.bias << f.mant
    return min(v, one)


def float_cmp(op: int, a: int, b: int) -> bool:
    if is_nan(a, F32) or is_nan(b, F32):
        return op in (0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F)
    x, y = from_f32(a), from_f32(b)
    table = {
        1: x < y,
        2: x == y,
        3: x <= y,
        4: x > y,
        5: x != y,
        6: x >= y,
        7: True,
        8: False,
        9: x < y,
        10: x == y,
        11: x <= y,
        12: x > y,
        13: x != y,
        14: x >= y,
        15: True,
        0: False,
    }
    return table[op]


def int_cmp(op: int, a: int, b: int, signed: bool) -> bool:
    if signed:
        a, b = [v - (1 << 32) if v & 0x80000000 else v for v in (a, b)]
    return {0: False, 1: a < b, 2: a == b, 3: a <= b, 4: a > b, 5: a != b, 6: a >= b, 7: True}[op]


def combine(op: int, x: bool, y: bool) -> bool:
    return (x and y, x or y, x != y)[op]


def mufu(op: int, x: int) -> int:
    v = from_f32(flush(x, F32))
    try:
        r = {
            4: lambda: 1 / v,
            5: lambda: 1 / math.sqrt(v),
            2: lambda: 2.0**v,
            3: lambda: math.log2(v),
            1: lambda: math.sin(v),
            0: lambda: math.cos(v),
            8: lambda: math.sqrt(v),
        }[op]()
    except (ZeroDivisionError, ValueError, OverflowError):
        r = {
            4: math.copysign(math.inf, v),
            5: math.inf if v == 0 else math.nan,
            2: math.inf,
            3: -math.inf if v == 0 else math.nan,
            8: math.nan,
        }.get(op, math.nan)
    return flush(to_f32(r), F32)
