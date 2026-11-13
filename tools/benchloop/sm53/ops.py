"""The ops the latency bench measures: how each forms a dependent link, the values it starts from, NAK's name for it.

A link writes `dst` from `src` (predicate indices for the setp ops) and reads its constants from K1 and K2. Every link
is injective on the values a chain meets, so one stale read anywhere changes the chain's result: chains of fresh
registers start each register at a poison value the true chain never holds. Constants sit in banks 1 and 2 and chain
registers in bank 0 (every fourth register from CHAIN0), so no link waits on a register-bank conflict.
"""

import math
from collections.abc import Callable
from dataclasses import dataclass

from tools.benchloop.sm53 import encode as e
from tools.benchloop.sm53.encode import PT, Imm, Instr, f32, half2
from tools.benchloop.sm53.operands import VARIABLE

K0 = 4
K1 = 5
K2 = 6
SRC = 7
CHAIN0 = 8
STRIDE = 4
HALF_ONE = half2(1.0, 1.0)
HALF_MINUS_ONE = half2(-1.0, -1.0)


@dataclass(frozen=True)
class Recipe:
    key: str
    nak: str
    link: Callable[[int, int], Instr]
    seed: int
    consts: tuple[int, int] = (0, 0)
    poison: int = 0
    step: int = 1
    exact: bool = True
    pred: bool = False

    @property
    def fixed(self) -> bool:
        return self.key.partition('.')[0] not in VARIABLE

    @property
    def slug(self) -> str:
        return self.key.lower().replace('.', '-')

    def constants(self) -> dict[int, int]:
        return {K1: self.consts[0], K2: self.consts[1]}


FIXED = (
    Recipe('MOV', 'Mov', e.mov, 0x12345678, poison=0xA5000000),
    Recipe('FADD', 'FAdd', lambda d, s: e.fadd(d, s, K1), f32(0.0), (f32(1.0), 0), f32(4096.25)),
    Recipe('FMUL', 'FMul', lambda d, s: e.fmul(d, s, K1), f32(1.0), (f32(2.0), 0), f32(3.0)),
    Recipe('FFMA', 'FFma', lambda d, s: e.ffma(d, s, K1, K2), f32(0.0), (f32(2.0), f32(1.0)), f32(0.25)),
    Recipe('FMNMX', 'FMnMx', lambda d, s: e.fmnmx(d, s, K1, 'max', 'neg_a'), f32(1.5), (f32(-math.inf), 0), f32(-2.5)),
    Recipe('HADD2', 'HAdd2', lambda d, s: e.hadd2(d, s, K1), 0, (HALF_ONE, 0), half2(-100.0, -100.0)),
    Recipe('HMUL2', 'HMul2', lambda d, s: e.hmul2(d, s, K1), half2(1.5, 2.5), (HALF_MINUS_ONE, 0), half2(3.0, 3.0)),
    Recipe(
        'HFMA2', 'HFma2', lambda d, s: e.hfma2(d, s, K1, K2), half2(0.25, 0.25), (HALF_MINUS_ONE, HALF_ONE), half2(8, 8)
    ),
    Recipe('IADD', 'IAdd2', lambda d, s: e.iadd(d, s, K1), 0, (1, 0), 0x40000000),
    Recipe('IADD32I', 'IAdd2', lambda d, s: e.iadd32i(d, s, 0x01010101), 0, poison=0x80000000),
    Recipe('LOP', 'Lop2', lambda d, s: e.lop(d, s, K1, 'xor'), 0x12345678, (0x5A5A5A5A, 0), 0xC0000000),
    Recipe('LOP32I', 'Lop2', lambda d, s: e.lop32i(d, s, 0x0F0F0F0F, 'xor'), 0x12345678, poison=0xC0000000),
    Recipe('SHL', 'Shl', lambda d, s: e.shl(d, s, Imm(1)), 1, poison=3, step=2),
    Recipe('SHR', 'Shr', lambda d, s: e.shr(d, s, Imm(1)), 0x80000000, poison=3, step=2),
    Recipe('IMNMX', 'IMnMx', lambda d, s: e.imnmx(d, s, K1, 'max', 's32'), 0x12345678, (0x80000000, 0), 0x60000000),
    Recipe('BFE', 'Bfe', lambda d, s: e.bfe(d, s, Imm(0x2000)), 0x12345678, poison=0x70000000),
    Recipe('SEL', 'Sel', lambda d, s: e.sel(d, s, K1), 0x12345678, (0xDEADBEEF, 0), 0x30000000),
    Recipe('PRMT', 'Prmt', lambda d, s: e.prmt(d, s, Imm(0x0321)), 0x12345678, poison=0x50000000),
    Recipe('FSETP', 'FSetP', lambda p, q: e.fsetp(p, K1, K2, 'lt xor', q), 0, (f32(1.0), f32(2.0)), pred=True),
    Recipe('ISETP', 'ISetP', lambda p, q: e.isetp(p, K1, K2, 'lt s32 xor', q), 0, (1, 2), pred=True),
)

VARIABLE_OPS = (
    Recipe('MUFU.RCP', 'MuFu', lambda d, s: e.mufu(d, s, 'rcp'), f32(3.0), exact=False),
    Recipe('MUFU.RSQ', 'MuFu', lambda d, s: e.mufu(d, s, 'rsq'), f32(3.0), exact=False),
    Recipe('MUFU.EX2', 'MuFu', lambda d, s: e.mufu(d, s, 'ex2', 'neg abs'), f32(1.0), exact=False),
    Recipe('MUFU.LG2', 'MuFu', lambda d, s: e.mufu(d, s, 'lg2', 'abs'), f32(3.0), exact=False),
    Recipe('MUFU.SIN', 'MuFu', lambda d, s: e.mufu(d, s, 'sin'), f32(1.0), exact=False),
    Recipe('MUFU.COS', 'MuFu', lambda d, s: e.mufu(d, s, 'cos'), f32(1.0), exact=False),
    Recipe('MUFU.SQRT', 'MuFu', lambda d, s: e.mufu(d, s, 'sqrt'), f32(3.0), exact=False),
    Recipe('RRO', 'Rro', e.rro, f32(1.0), exact=False),
    Recipe('F2F', 'F2F', lambda d, s: e.f2f(d, s, 'round'), f32(2.5)),
    Recipe('F2I', 'F2I', lambda d, s: e.f2i(d, s, 'rz'), f32(1.5e9)),
    Recipe('I2F', 'I2F', e.i2f, 0x12345678),
    Recipe('IMUL', 'IMul', lambda d, s: e.imul(d, s, K1), 0x12345679, (3, 0)),
    Recipe('FLO', 'Flo', e.flo, 0x40000000),
    # c[1][0] is the first constant (0): every link loads address 0 into the chain register
    Recipe('LDC', 'Ldc', lambda d, s: e.ldc(d, s, 1), 0),
)

RECIPES = {r.key: r for r in FIXED + VARIABLE_OPS}

# Independent instructions for the dual-issue pairs: they read K1, K2 and SRC only
PAIR_FORMS: dict[str, Callable[[int], Instr]] = {
    'FFMA': lambda d: e.ffma(d, K1, K2, SRC),
    'FADD': lambda d: e.fadd(d, K1, K2),
    'FMUL': lambda d: e.fmul(d, K1, K2),
    'FMNMX': lambda d: e.fmnmx(d, K1, K2, 'min'),
    'HFMA2': lambda d: e.hfma2(d, K1, K2, SRC),
    'IADD': lambda d: e.iadd(d, K1, K2),
    'LOP': lambda d: e.lop(d, K1, K2, 'xor'),
    'SHL': lambda d: e.shl(d, K1, Imm(3)),
    'MOV': lambda d: e.mov(d, K1),
    'MOV32I': lambda d: e.mov32i(d, 0x3F000000),
    'SEL': lambda d: e.sel(d, K1, K2, PT),
    'ISETP': lambda _: e.isetp(0, K1, K2, 'lt s32'),
    'MUFU': lambda d: e.mufu(d, K1, 'rcp'),
    'I2F': lambda d: e.i2f(d, K1),
}
PAIRS = (
    ('FFMA', 'FFMA'),
    ('FFMA', 'FADD'),
    ('FFMA', 'IADD'),
    ('FFMA', 'LOP'),
    ('FFMA', 'SHL'),
    ('FFMA', 'MOV'),
    ('FFMA', 'MOV32I'),
    ('FFMA', 'SEL'),
    ('FFMA', 'ISETP'),
    ('FFMA', 'HFMA2'),
    ('FFMA', 'MUFU'),
    ('FFMA', 'I2F'),
    ('IADD', 'LOP'),
    ('IADD', 'IADD'),
    ('MOV', 'MOV'),
    ('HFMA2', 'HFMA2'),
)


def chain_registers(n: int) -> list[int]:
    return [CHAIN0 + STRIDE * j for j in range(n)]
