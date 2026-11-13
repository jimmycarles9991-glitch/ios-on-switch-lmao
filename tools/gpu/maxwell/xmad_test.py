"""XMAD in the interpreter, and NAK's SM50 multiply sequences (sm50/xmad.rs) run through it against exact products."""

import random
import unittest
from dataclasses import dataclass

from tools.gpu.maxwell import isa
from tools.gpu.maxwell.execute import alu
from tools.gpu.maxwell.interp import Inputs, Lane

A, B, C, D = 1, 2, 3, 4
NO_INPUTS = Inputs(attribute=lambda _: 0, cbuf=lambda _b, _o: 0, texture=lambda _k: [0] * 4)


@dataclass(frozen=True)
class Form:
    imm: int | None = None
    a_hi: bool = False
    b_hi: bool = False
    psl: bool = False
    mrg: bool = False
    cbcc: bool = False


def encode(f: Form) -> int:
    """Encode an XMAD writing r4 from r1, r2 (or a 16-bit immediate) and r3, in nv50_ir emitXMAD's layout."""
    w = (0x3600 if f.imm is not None else 0x5B00) << 48
    w |= f.imm << 20 if f.imm is not None else B << 20 | int(f.b_hi) << 35
    w |= D | A << 8 | C << 39 | int(f.psl) << 36 | int(f.mrg) << 37 | (4 if f.cbcc else 0) << 50 | int(f.a_hi) << 53
    return w


def xmad(a: int, b: int, c: int, **form: bool | int) -> int:
    lane = Lane()
    lane.w(A, a)
    lane.w(B, b)
    lane.w(C, c)
    w = encode(Form(**form))
    alu(isa.name(w), w, lane, NO_INPUTS)
    return lane.r(D)


def imul(x: int, y: int) -> int:
    lo = xmad(y, x, 0)
    merged = xmad(y, x, 0, b_hi=True, mrg=True)
    return xmad(y, merged, lo, a_hi=True, b_hi=True, psl=True, cbcc=True)


def imul_imm(x: int, k: int) -> int:
    lo = xmad(x, 0, 0, imm=k & 0xFFFF)
    low = xmad(x, 0, lo, imm=k & 0xFFFF, a_hi=True, psl=True)
    return low if k >> 16 == 0 else xmad(x, 0, low, imm=k >> 16, psl=True)


def umul64(x: int, y: int) -> tuple[int, int]:
    ll = xmad(x, y, 0)
    t = xmad(x, y, ll >> 16, a_hi=True)
    u = xmad(x, y, t & 0xFFFF, b_hi=True)
    v = xmad(x, y, t >> 16, a_hi=True, b_hi=True)
    return (ll & 0xFFFF) | (u & 0xFFFF) << 16, (v + (u >> 16)) & 0xFFFFFFFF


def umul64_short(x: int, k: int) -> tuple[int, int]:
    ll = xmad(x, 0, 0, imm=k)
    t = xmad(x, 0, ll >> 16, imm=k, a_hi=True)
    return xmad(x, 0, ll, imm=k, a_hi=True, psl=True), t >> 16


def signed(v: int) -> int:
    return v - (1 << 32) if v >> 31 else v


class XmadTest(unittest.TestCase):
    def test_decodes(self):
        self.assertEqual(isa.name(encode(Form())), 'XMAD_reg')
        self.assertEqual(isa.name(encode(Form(imm=0xFFFF, psl=True))), 'XMAD_imm')

    def test_forms(self):
        rng = random.Random(7)
        for _ in range(2000):
            a, b, c = (rng.getrandbits(32) for _ in range(3))
            self.assertEqual(xmad(a, b, c), ((a & 0xFFFF) * (b & 0xFFFF) + c) & 0xFFFFFFFF)
            self.assertEqual(xmad(a, b, c, a_hi=True, b_hi=True), ((a >> 16) * (b >> 16) + c) & 0xFFFFFFFF)
            self.assertEqual(xmad(a, b, c, psl=True), ((a & 0xFFFF) * (b & 0xFFFF) << 16) + c & 0xFFFFFFFF)
            self.assertEqual(xmad(a, b, c, cbcc=True), ((a & 0xFFFF) * (b & 0xFFFF) + c + (b << 16)) & 0xFFFFFFFF)
            merged = xmad(a, b, c, mrg=True)
            self.assertEqual(merged, ((a & 0xFFFF) * (b & 0xFFFF) + c) & 0xFFFF | (b & 0xFFFF) << 16)

    def test_products(self):
        rng = random.Random(8)
        edges = [0, 1, 0xFFFF, 0x10000, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x80808081, 0x00FFFFFF]
        pairs = [(x, y) for x in edges for y in edges]
        pairs += [(rng.getrandbits(32), rng.getrandbits(32)) for _ in range(3000)]
        for x, y in pairs:
            self.assertEqual(imul(x, y), x * y & 0xFFFFFFFF, (hex(x), hex(y)))
            self.assertEqual(imul_imm(x, y), x * y & 0xFFFFFFFF, (hex(x), hex(y)))
            lo, hi = umul64(x, y)
            self.assertEqual(hi << 32 | lo, x * y, (hex(x), hex(y)))
            if y <= 0xFFFF:
                self.assertEqual(umul64_short(x, y), (lo, hi), (hex(x), hex(y)))
            s_hi = (hi - (y if x >> 31 else 0) - (x if y >> 31 else 0)) & 0xFFFFFFFF
            self.assertEqual(s_hi << 32 | lo, signed(x) * signed(y) & 0xFFFFFFFFFFFFFFFF, (hex(x), hex(y)))


if __name__ == '__main__':
    unittest.main()
