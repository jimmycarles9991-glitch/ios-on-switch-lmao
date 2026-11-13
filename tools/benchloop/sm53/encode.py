"""SM50/SM53 machine code for the latency bench: isa.py's opcode bits, the fields NAK's sm50.rs writes.

Registers are ints (RZ 255), constant-buffer operands Cbuf, immediates Imm (a 32-bit pattern; the 20-bit forms take
the integer's low 20 bits or the float's top 20) and predicate operands an int or Pred. Modifiers are one
space-separated string, e.g. ffma(8, 8, 5, 6, 'neg_c rz'). Fields that are not operands carry the values NAK writes
(lane masks 0xf, unused predicate destinations PT, unused registers RZ), so a constructor's word is NAK's word.
"""

import struct
from collections.abc import Iterable
from dataclasses import dataclass, field, replace
from typing import Self

from tools.gpu.maxwell import isa

RZ = 255
PT = 7
NO_BAR = 7
ALL_BARRIERS = 0x3F
# A barrier an instruction sets is not up until this many cycles after it issues: a wait on it sooner passes at once
# and reads the register before the write lands (measured: IPA then a waiting MOV, FADD or F2I at delay 1; NAK's
# calc_instr_deps keeps the same two cycles)
BARRIER_SET_CYCLES = 2
MASK32 = 0xFFFFFFFF
OPCODES = {p.name: p.value for p in isa.PATTERNS}
ROUND = {'rn': 0, 'rm': 1, 'rp': 2, 'rz': 3}
FLOAT_CMP = {'f': 0, 'lt': 1, 'eq': 2, 'le': 3, 'gt': 4, 'ne': 5, 'ge': 6, 'num': 7, 'nan': 8}
FLOAT_CMP |= {'ltu': 9, 'equ': 10, 'leu': 11, 'gtu': 12, 'neu': 13, 'geu': 14, 't': 15}
INT_CMP = {'f': 0, 'lt': 1, 'eq': 2, 'le': 3, 'gt': 4, 'ne': 5, 'ge': 6, 't': 7}
COMBINE = {'and': 0, 'or': 1, 'xor': 2}
LOGIC = {'and': 0, 'or': 1, 'xor': 2, 'passb': 3}
MUFU = {'cos': 0, 'sin': 1, 'ex2': 2, 'lg2': 3, 'rcp': 4, 'rsq': 5, 'sqrt': 8}
SWIZZLE = {'h1h0': 0, 'f32': 1, 'h0h0': 2, 'h1h1': 3}
FLOAT_WIDTH = {'f16': 1, 'f32': 2}
FREQ = {'pass': 0, 'mulw': 1, 'const': 2, 'state': 3}
LOCATION = {'center': 0, 'centroid': 1, 'offset': 2}


@dataclass(frozen=True)
class Control:
    """One instruction's 21 scheduling bits (sm50.rs set_instr_deps)."""

    delay: int = 1
    yld: bool = False
    wr_bar: int = NO_BAR
    rd_bar: int = NO_BAR
    wait: int = 0
    reuse: int = 0

    def bits(self) -> int:
        for value, width, what in ((self.delay, 4, 'delay'), (self.wr_bar, 3, 'wr_bar'), (self.rd_bar, 3, 'rd_bar')):
            if not 0 <= value < 1 << width:
                raise ValueError(f'{what} {value} does not fit {width} bits')
        if not (0 <= self.wait <= ALL_BARRIERS and 0 <= self.reuse < 16):
            raise ValueError(f'wait {self.wait:#x} / reuse {self.reuse:#x} out of range')
        return (
            self.delay | int(self.yld) << 4 | self.wr_bar << 5 | self.rd_bar << 8 | self.wait << 11 | self.reuse << 17
        )

    @classmethod
    def of(cls, bits: int) -> Self:
        return cls(
            delay=bits & 0xF,
            yld=bool(bits >> 4 & 1),
            wr_bar=bits >> 5 & 7,
            rd_bar=bits >> 8 & 7,
            wait=bits >> 11 & ALL_BARRIERS,
            reuse=bits >> 17 & 0xF,
        )


@dataclass(frozen=True)
class Instr:
    word: int
    control: Control = field(default_factory=Control)

    @property
    def name(self) -> str:
        return isa.name(self.word)

    def sched(self, **fields: int | bool) -> Self:
        """Give this instruction with the named Control fields replaced."""
        return replace(self, control=replace(self.control, **fields))


@dataclass(frozen=True)
class Cbuf:
    bank: int
    offset: int


@dataclass(frozen=True)
class Imm:
    value: int


@dataclass(frozen=True)
class Pred:
    index: int
    neg: bool = False


Operand = int | Cbuf | Imm


def f32(x: float) -> int:
    return struct.unpack('<I', struct.pack('<f', x))[0]


def half2(lo: float, hi: float) -> int:
    return struct.unpack('<I', struct.pack('<ee', lo, hi))[0]


class _Word:
    def __init__(self, name: str) -> None:
        self.name = name
        self.value = OPCODES[name] | PT << 16

    def put(self, at: int, n: int, v: int) -> Self:
        if not 0 <= v < 1 << n:
            raise ValueError(f'{self.name}: {v:#x} does not fit bits {at}..{at + n - 1}')
        self.value |= v << at
        return self

    def flag(self, at: int, on: bool) -> Self:
        return self.put(at, 1, int(on))

    def reg(self, at: int, r: int) -> Self:
        return self.put(at, 8, r)

    def pred(self, at: int, p: int | Pred) -> Self:
        p = p if isinstance(p, Pred) else Pred(p)
        return self.put(at, 3, p.index).flag(at + 3, p.neg)

    def cbuf(self, cb: Cbuf) -> Self:
        if cb.offset % 4:
            raise ValueError(f'{self.name}: c[{cb.bank}][{cb.offset:#x}] is not word aligned')
        return self.put(20, 14, cb.offset >> 2).put(34, 5, cb.bank)

    def fmod(self, abs_at: int, neg_at: int, m: set[str], which: str) -> Self:
        return self.flag(abs_at, f'abs_{which}' in m).flag(neg_at, f'neg_{which}' in m)

    def instr(self) -> Instr:
        return Instr(self.value)


def _mods(text: str, allowed: Iterable[str]) -> set[str]:
    m = set(text.split())
    unknown = m - set(allowed)
    if unknown:
        raise ValueError(f'unknown modifiers {sorted(unknown)}')
    return m


def _pick(m: set[str], options: dict[str, int], default: int = 0) -> int:
    chosen = m & options.keys()
    if len(chosen) > 1:
        raise ValueError(f'conflicting modifiers {sorted(chosen)}')
    return options[chosen.pop()] if chosen else default


def _prefixed(prefix: str, table: dict[str, int]) -> dict[str, int]:
    return {f'{prefix}={k}': v for k, v in table.items()}


def _form(base: str, b: Operand, imm: str | None = 'i20', cbuf: str = 'cbuf') -> _Word:
    """Give the word of `base`'s register, constant-buffer or 20-bit immediate form with operand B in place."""
    if isinstance(b, Cbuf):
        return _Word(f'{base}_{cbuf}').cbuf(b)
    if not isinstance(b, Imm):
        return _Word(f'{base}_reg').reg(20, b)
    v = b.value & MASK32
    w = _Word(f'{base}_imm')
    if imm == 'f20':
        if v & 0xFFF:
            raise ValueError(f'{base}: {v:#x} is not a 20-bit float immediate')
        return w.put(20, 19, v >> 12 & 0x7FFFF).put(56, 1, v >> 31)
    if imm != 'i20' or v & 0xFFF80000 not in (0, 0xFFF80000):
        raise ValueError(f'{base}: {v:#x} is not a 20-bit integer immediate')
    return w.put(20, 19, v & 0x7FFFF).put(56, 1, v >> 19 & 1)


def _fmod_b(w: _Word, b: Operand, m: set[str], abs_at: int, neg_at: int) -> _Word:
    if isinstance(b, Imm):
        if m & {'abs_b', 'neg_b'}:
            raise ValueError(f'{w.name}: an immediate takes no modifiers')
        return w
    return w.fmod(abs_at, neg_at, m, 'b')


def mov(d: int, b: int | Cbuf) -> Instr:
    return _form('MOV', b, imm=None).put(39, 4, 0xF).reg(0, d).instr()


def mov32i(d: int, value: int) -> Instr:
    return _Word('MOV32I').reg(0, d).put(20, 32, value & MASK32).put(12, 4, 0xF).instr()


def fadd(d: int, a: int, b: Operand, mods: str = '') -> Instr:
    m = _mods(mods, {'neg_a', 'abs_a', 'neg_b', 'abs_b', 'ftz', 'sat', *ROUND})
    w = _fmod_b(_form('FADD', b, 'f20'), b, m, 49, 45).reg(0, d).reg(8, a).fmod(46, 48, m, 'a')
    return w.put(39, 2, _pick(m, ROUND)).flag(44, 'ftz' in m).flag(50, 'sat' in m).instr()


def fmul(d: int, a: int, b: Operand, mods: str = '') -> Instr:
    m = _mods(mods, {'neg', 'ftz', 'dnz', 'sat', *ROUND})
    w = _form('FMUL', b, 'f20').reg(0, d).reg(8, a).put(39, 2, _pick(m, ROUND))
    return w.flag(44, 'ftz' in m).flag(45, 'dnz' in m).flag(48, 'neg' in m).flag(50, 'sat' in m).instr()


def ffma(d: int, a: int, b: Operand, c: int | Cbuf, mods: str = '') -> Instr:
    """Give d = a * b + c; a constant-buffer c takes the rc form, whose register B sits in C's field."""
    m = _mods(mods, {'neg_ab', 'neg_c', 'sat', 'ftz', 'dnz', *ROUND})
    if isinstance(c, Cbuf):
        if not isinstance(b, int):
            raise ValueError('FFMA with a constant-buffer C takes a register B')
        w = _Word('FFMA_rc').cbuf(c).reg(39, b)
    else:
        w = _form('FFMA', b, 'f20', cbuf='cr').reg(39, c)
    w = w.reg(0, d).reg(8, a).flag(48, 'neg_ab' in m).flag(49, 'neg_c' in m).flag(50, 'sat' in m)
    return w.put(51, 2, _pick(m, ROUND)).flag(53, 'ftz' in m).flag(54, 'dnz' in m).instr()


def _minmax(op: str) -> Pred:
    if op not in ('min', 'max'):
        raise ValueError(f'min or max, not {op!r}')
    return Pred(PT, neg=op == 'max')


def fmnmx(d: int, a: int, b: Operand, op: str, mods: str = '') -> Instr:
    m = _mods(mods, {'neg_a', 'abs_a', 'neg_b', 'abs_b', 'ftz'})
    w = _fmod_b(_form('FMNMX', b, 'f20'), b, m, 49, 45).reg(0, d).reg(8, a).fmod(46, 48, m, 'a')
    return w.pred(39, _minmax(op)).flag(44, 'ftz' in m).instr()


def fsetp(p: int, a: int, b: Operand, test: str, accum: int | Pred = PT) -> Instr:
    """Give p = (a <cmp> b) <combine> accum; `test` is the comparison then modifiers, e.g. 'lt xor ftz'."""
    cmp, *rest = test.split()
    m = _mods(' '.join(rest), {'neg_a', 'abs_a', 'neg_b', 'abs_b', 'ftz', *COMBINE})
    w = _fmod_b(_form('FSETP', b, 'f20'), b, m, 44, 6).put(0, 3, PT).put(3, 3, p).reg(8, a).fmod(7, 43, m, 'a')
    w = w.pred(39, accum).put(45, 2, _pick(m, COMBINE)).flag(47, 'ftz' in m)
    return w.put(48, 4, FLOAT_CMP[cmp]).instr()


def _half_mods(extra: Iterable[str]) -> set[str]:
    return {*extra, *_prefixed('a', SWIZZLE), *_prefixed('b', SWIZZLE), 'out=f32', 'sat'}


def _half(name: str, d: int, a: int, b: int, m: set[str]) -> _Word:
    w = _Word(name).reg(0, d).reg(8, a).reg(20, b).put(28, 2, _pick(m, _prefixed('b', SWIZZLE)))
    return w.flag(32, 'sat' in m).put(47, 2, _pick(m, _prefixed('a', SWIZZLE))).put(49, 2, int('out=f32' in m))


def _half_fmz(m: set[str]) -> int:
    return 2 if 'fmz' in m else int('ftz' in m)


def hadd2(d: int, a: int, b: int, mods: str = '') -> Instr:
    m = _mods(mods, _half_mods({'neg_a', 'abs_a', 'neg_b', 'abs_b', 'ftz'}))
    w = _half('HADD2_reg', d, a, b, m).flag(30, 'abs_b' in m).flag(31, 'neg_b' in m).flag(39, 'ftz' in m)
    return w.flag(43, 'neg_a' in m).flag(44, 'abs_a' in m).instr()


def hmul2(d: int, a: int, b: int, mods: str = '') -> Instr:
    m = _mods(mods, _half_mods({'neg', 'abs_a', 'abs_b', 'ftz', 'fmz'}))
    w = _half('HMUL2_reg', d, a, b, m).flag(30, 'abs_b' in m).flag(31, 'neg' in m).put(39, 2, _half_fmz(m))
    return w.flag(44, 'abs_a' in m).instr()


def hfma2(d: int, a: int, b: int, c: int, mods: str = '') -> Instr:
    m = _mods(mods, _half_mods({'neg', 'neg_c', 'ftz', 'fmz', *_prefixed('c', SWIZZLE)}))
    w = _half('HFMA2_reg', d, a, b, m).flag(30, 'neg_c' in m).flag(31, 'neg' in m)
    return w.put(35, 2, _pick(m, _prefixed('c', SWIZZLE))).put(37, 2, _half_fmz(m)).reg(39, c).instr()


def mufu(d: int, a: int, func: str, mods: str = '') -> Instr:
    m = _mods(mods, {'neg', 'abs'})
    w = _Word('MUFU').reg(0, d).reg(8, a).flag(46, 'abs' in m).flag(48, 'neg' in m)
    return w.put(20, 4, MUFU[func]).instr()


def rro(d: int, b: Operand, mods: str = '') -> Instr:
    m = _mods(mods, {'ex2', 'neg_b', 'abs_b'})
    return _fmod_b(_form('RRO', b, 'f20'), b, m, 49, 45).reg(0, d).flag(39, 'ex2' in m).instr()


def f2f(d: int, b: Operand, mods: str = '') -> Instr:
    """Give a float conversion; 'round' rounds to an integral value of the same width (NAK's FRND)."""
    widths = {**_prefixed('dst', FLOAT_WIDTH), **_prefixed('src', FLOAT_WIDTH)}
    m = _mods(mods, {*widths, 'round', 'ftz', 'neg_b', 'abs_b', 'high', *ROUND})
    w = _fmod_b(_form('F2F', b), b, m, 49, 45).reg(0, d)
    w = w.put(8, 2, _pick(m, _prefixed('dst', FLOAT_WIDTH), 2)).put(10, 2, _pick(m, _prefixed('src', FLOAT_WIDTH), 2))
    return w.put(39, 2, _pick(m, ROUND)).flag(41, 'high' in m).flag(42, 'round' in m).flag(44, 'ftz' in m).instr()


def f2i(d: int, b: Operand, mods: str = '') -> Instr:
    m = _mods(mods, {'u32', 'src=f16', 'ftz', 'neg_b', 'abs_b', 'high', *ROUND})
    w = _fmod_b(_form('F2I', b, 'f20'), b, m, 49, 45).reg(0, d).put(8, 2, 2).put(10, 2, 1 if 'src=f16' in m else 2)
    w = w.flag(12, 'u32' not in m).put(39, 2, _pick(m, ROUND)).flag(41, 'high' in m)
    return w.flag(44, 'ftz' in m).instr()


def i2f(d: int, b: Operand, mods: str = '') -> Instr:
    m = _mods(mods, {'u32', 'dst=f16', 'neg_b', *ROUND})
    w = _form('I2F', b).reg(0, d).put(8, 2, 1 if 'dst=f16' in m else 2).put(10, 2, 2).flag(13, 'u32' not in m)
    if 'neg_b' in m and isinstance(b, Imm):
        raise ValueError('I2F: an immediate takes no modifiers')
    return w.flag(45, 'neg_b' in m).put(39, 2, _pick(m, ROUND)).instr()


def iadd(d: int, a: int, b: Operand, mods: str = '') -> Instr:
    m = _mods(mods, {'neg_a', 'neg_b'})
    if m == {'neg_a', 'neg_b'} or (isinstance(b, Imm) and 'neg_b' in m):
        raise ValueError('IADD: one negated register operand at most, never a negated immediate')
    return _form('IADD', b).reg(0, d).reg(8, a).flag(48, 'neg_b' in m).flag(49, 'neg_a' in m).instr()


def iadd32i(d: int, a: int, value: int, mods: str = '') -> Instr:
    m = _mods(mods, {'neg_a'})
    return _Word('IADD32I').reg(0, d).reg(8, a).put(20, 32, value & MASK32).flag(56, 'neg_a' in m).instr()


def imul(d: int, a: int, b: Operand, mods: str = '') -> Instr:
    m = _mods(mods, {'hi', 'signed_a', 'signed_b'})
    w = _form('IMUL', b).reg(0, d).reg(8, a).flag(39, 'hi' in m)
    return w.flag(40, 'signed_a' in m).flag(41, 'signed_b' in m).instr()


def lop(d: int, a: int, b: Operand, op: str, mods: str = '') -> Instr:
    m = _mods(mods, {'inv_a', 'inv_b'})
    if isinstance(b, Imm) and 'inv_b' in m:
        raise ValueError('LOP: an immediate takes no modifiers')
    w = _form('LOP', b).reg(0, d).reg(8, a).flag(39, 'inv_a' in m).flag(40, 'inv_b' in m)
    return w.put(41, 2, LOGIC[op]).put(48, 3, PT).instr()


def lop32i(d: int, a: int, value: int, op: str, mods: str = '') -> Instr:
    m = _mods(mods, {'inv_a', 'inv_b'})
    if op == 'passb':
        raise ValueError('LOP32I has no PASS_B')
    w = _Word('LOP32I').reg(0, d).reg(8, a).put(20, 32, value & MASK32).put(53, 2, LOGIC[op])
    return w.flag(55, 'inv_a' in m).flag(56, 'inv_b' in m).instr()


def shl(d: int, a: int, b: Operand, mods: str = '') -> Instr:
    m = _mods(mods, {'wrap'})
    return _form('SHL', b).reg(0, d).reg(8, a).flag(39, 'wrap' in m).instr()


def shr(d: int, a: int, b: Operand, mods: str = '') -> Instr:
    m = _mods(mods, {'wrap', 's32'})
    return _form('SHR', b).reg(0, d).reg(8, a).flag(39, 'wrap' in m).flag(48, 's32' in m).instr()


def imnmx(d: int, a: int, b: Operand, op: str, mods: str = '') -> Instr:
    m = _mods(mods, {'s32'})
    return _form('IMNMX', b).reg(0, d).reg(8, a).pred(39, _minmax(op)).flag(48, 's32' in m).instr()


def bfe(d: int, a: int, b: Operand, mods: str = '') -> Instr:
    """Give a bit-field extract; B holds the position in bits 0-7 and the length in bits 8-15."""
    m = _mods(mods, {'s32', 'rev'})
    return _form('BFE', b).reg(0, d).reg(8, a).flag(40, 'rev' in m).flag(48, 's32' in m).instr()


def flo(d: int, b: Operand, mods: str = '') -> Instr:
    m = _mods(mods, {'inv_b', 'shift', 's32'})
    w = _form('FLO', b).reg(0, d).flag(40, 'inv_b' in m).flag(41, 'shift' in m)
    return w.flag(48, 's32' in m).instr()


def sel(d: int, a: int, b: Operand, pred: int | Pred = PT) -> Instr:
    return _form('SEL', b).reg(0, d).reg(8, a).pred(39, pred).instr()


def prmt(d: int, a: int, selector: Operand, c: int = RZ) -> Instr:
    return _form('PRMT', selector, cbuf='cr').reg(0, d).reg(8, a).reg(39, c).instr()


def isetp(p: int, a: int, b: Operand, test: str, accum: int | Pred = PT) -> Instr:
    """Give p = (a <cmp> b) <combine> accum; `test` is the comparison then modifiers, e.g. 'lt s32 xor'."""
    cmp, *rest = test.split()
    m = _mods(' '.join(rest), {'s32', *COMBINE})
    w = _form('ISETP', b).put(0, 3, PT).put(3, 3, p).reg(8, a).pred(39, accum)
    return w.put(45, 2, _pick(m, COMBINE)).flag(48, 's32' in m).put(49, 3, INT_CMP[cmp]).instr()


def ipa(d: int, addr: int, mods: str = '', inv_w: int = RZ) -> Instr:
    """Give an attribute interpolation at byte address `addr` ('pass' unless 'mulw', 'const' or 'state')."""
    m = _mods(mods, {*FREQ, *LOCATION, 'sat'})
    if addr % 4:
        raise ValueError(f'IPA: attribute address {addr:#x} is not word aligned')
    w = _Word('IPA').reg(0, d).reg(8, RZ).reg(20, inv_w).put(28, 10, addr).reg(39, RZ).put(47, 3, PT)
    return w.flag(51, 'sat' in m).put(52, 2, _pick(m, LOCATION)).put(54, 2, _pick(m, FREQ)).instr()


def bra(offset: int, pred: int = PT, neg: bool = False) -> Instr:
    """Give a branch `offset` bytes from the next instruction, guarded by `pred`."""
    word = OPCODES['BRA'] | (pred | int(neg) << 3) << 16 | (offset & 0xFFFFFF) << 20 | 0xF
    return Instr(word)


def address(index: int) -> int:
    """Give the byte address of the index-th instruction once bundle() puts a scheduling word ahead of each three."""
    return index // 3 * 32 + 8 + index % 3 * 8


def ldc(d: int, offset_reg: int, bank: int, offset: int = 0, size: int = 4) -> Instr:
    """LDC as sm50.rs encodes it; `size` is the memory type (4: 32-bit, 5: 64-bit; 6, 128-bit, traps on GM20B)."""
    return Instr(OPCODES['LDC'] | d | offset_reg << 8 | PT << 16 | offset << 20 | bank << 36 | size << 48)


def nop() -> Instr:
    return _Word('NOP').put(8, 4, 0xF).instr()


def exit_() -> Instr:
    return _Word('EXIT').put(0, 4, 0xF).instr()


def finish(instrs: list[Instr]) -> list[Instr]:
    """Give `instrs` ended as NAK ends a program: EXIT once every barrier is clear, then a NOP two cycles on."""
    return [*instrs, exit_().sched(delay=13, wait=ALL_BARRIERS), nop().sched(delay=2)]


def barrier_hazards(instrs: list[Instr]) -> list[int]:
    """Give the indices of instructions that wait on a barrier the instruction just before set too recently."""
    out = []
    for k in range(1, len(instrs)):
        before, c = instrs[k - 1].control, instrs[k].control
        set_bars = {b for b in (before.wr_bar, before.rd_bar) if b != NO_BAR}
        if before.delay < BARRIER_SET_CYCLES and any(c.wait >> b & 1 for b in set_bars):
            out.append(k)
    return out


def bundle(instrs: list[Instr]) -> bytes:
    """Give `instrs` as machine code: a scheduling word ahead of each three, NOPs filling the last bundle."""
    if hazards := barrier_hazards(instrs):
        raise ValueError(f'instructions {hazards} wait on a barrier set less than {BARRIER_SET_CYCLES} cycles before')
    padded = list(instrs) + [nop()] * (-len(instrs) % 3)
    words = []
    for i in range(0, len(padded), 3):
        group = padded[i : i + 3]
        words.append(sum(g.control.bits() << (21 * k) for k, g in enumerate(group)))
        words.extend(g.word for g in group)
    return struct.pack(f'<{len(words)}Q', *words)


def unbundle(code: bytes) -> list[Instr]:
    words = struct.unpack(f'<{len(code) // 8}Q', code[: len(code) // 8 * 8])
    out = []
    for i in range(0, len(words) - len(words) % 4, 4):
        sched = words[i]
        out.extend(Instr(words[i + 1 + k], Control.of(sched >> (21 * k) & 0x1FFFFF)) for k in range(3))
    return out
