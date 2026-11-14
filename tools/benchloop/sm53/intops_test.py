import random
import unittest

from tools.benchloop.sm53 import encode as e, intops, memops
from tools.benchloop.sm53.encode import RZ, Cbuf, Imm
from tools.gpu.maxwell import isa
from tools.gpu.maxwell.execute import alu
from tools.gpu.maxwell.interp import Inputs, Lane

NO_INPUTS = Inputs(attribute=lambda _: 0, cbuf=lambda _b, _o: 0, texture=lambda _k: [0] * 4)
A, B, C, D = 1, 2, 3, 4

# Words NAK wrote for the pixel interlock in a shipped pack (x1-pack-harvest ios27): its leader election, ticket
# wait and release
NAK_WORDS = {
    0xF0C8000000070000: memops.s2r(0, memops.SR_LANE_ID),
    0xF0C8000001370001: memops.s2r(1, memops.SR_THREAD_KILL),
    0xF0C8000001C70001: memops.s2r(1, memops.SR_AFFINITY),
    0xF0C8000000F70002: memops.s2r(2, memops.SR_ORDERING_TICKET),
    0x50D9E00000070001: memops.vote_any(1, e.PT, 0),
    0x5C30000000170001: e.flo(1, 1),
    0x4C10800009870101: memops.carry_out(e.iadd(1, 1, Cbuf(0, 0x260))),
    0x4C1008000997FF03: memops.carry_in(e.iadd(3, RZ, Cbuf(0, 0x264))),
    0xED01000000470601: memops.atom_add(1, 6, 4),
    0xEBF9000000070600: memops.red_add(6, 0),
    0xEF98000000070100: memops.membar_gpu(),
}


def run(instr: e.Instr, a: int, b: int, c: int) -> int:
    lane = Lane()
    for r, v in ((A, a), (B, b), (C, c)):
        lane.w(r, v)
    alu(isa.name(instr.word), instr.word, lane, NO_INPUTS)
    return lane.r(D)


def lo(v: int) -> int:
    return v & 0xFFFF


def hi(v: int) -> int:
    return v >> 16


def field(w: int, at: int, n: int) -> int:
    return w >> at & (1 << n) - 1


class NakWordsTest(unittest.TestCase):
    def test_interlock_words(self):
        for word, instr in NAK_WORDS.items():
            self.assertEqual(instr.word, word, f'{instr.name}: {instr.word:016x} != {word:016x}')

    def test_names(self):
        self.assertEqual(memops.ldg(4, 6).name, 'LDG')
        self.assertEqual(memops.stg(6, 4).name, 'STG')
        self.assertEqual(intops.xmad(D, A, Imm(3), C).name, 'XMAD_imm')
        self.assertEqual(intops.lop3(D, A, Imm(3), C, 0x96).name, 'LOP3_imm')
        self.assertEqual(intops.iadd3(D, A, B, C).name, 'IADD3_reg')
        self.assertEqual(intops.iscadd(D, A, Imm(5), 2).name, 'ISCADD_imm')

    def test_ldg_cache_and_width(self):
        w = memops.ldg(4, 6, 'cg').word
        self.assertEqual((field(w, 45, 1), field(w, 46, 2), field(w, 48, 3)), (1, 1, 4))

    def test_guard(self):
        w = memops.guarded(memops.atom_add(1, 6, RZ), 3, neg=True).word
        self.assertEqual(field(w, 16, 4), 0b1011)

    def test_odd_address_refused(self):
        with self.assertRaises(ValueError):
            memops.atom_add(1, 5, 4)


class SemanticsTest(unittest.TestCase):
    """The encoders' words through the interpreter's xmad() and three_source() (Ryujinx's layouts) against Python."""

    def test_xmad_forms(self):
        rng = random.Random(3)
        for _ in range(500):
            a, b, c = (rng.getrandbits(32) for _ in range(3))
            cases = {
                '': lo(a) * lo(b) + c,
                'a_hi b_hi': hi(a) * hi(b) + c,
                'psl': (lo(a) * lo(b) << 16) + c,
                'chi': lo(a) * lo(b) + hi(c),
                'clo': lo(a) * lo(b) + lo(c),
                'a_hi b_hi psl cbcc': (hi(a) * hi(b) << 16) + c + (b << 16),
            }
            for mods, want in cases.items():
                self.assertEqual(run(intops.xmad(D, A, B, C, mods), a, b, c), want & 0xFFFFFFFF, mods)
            merged = run(intops.xmad(D, A, B, C, 'mrg'), a, b, c)
            self.assertEqual(merged, (lo(a) * lo(b) + c) & 0xFFFF | lo(b) << 16)
            self.assertEqual(run(intops.xmad(D, A, Imm(0xBEEF), C), a, b, c), (lo(a) * 0xBEEF + c) & 0xFFFFFFFF)

    def test_three_source(self):
        rng = random.Random(4)
        for _ in range(500):
            a, b, c = (rng.getrandbits(32) for _ in range(3))
            self.assertEqual(run(intops.lop3(D, A, B, C, intops.LUT_XOR3), a, b, c), a ^ b ^ c)
            self.assertEqual(run(intops.lop3(D, A, B, C, 0xE8), a, b, c), (a & b) | (a & c) | (b & c))
            self.assertEqual(run(intops.lop3(D, A, Imm(0x1234), C, 0x96), a, b, c), a ^ 0x1234 ^ c)
            self.assertEqual(run(intops.iadd3(D, A, B, C), a, b, c), (a + b + c) & 0xFFFFFFFF)
            self.assertEqual(run(intops.iadd3(D, A, B, C, 'neg_b'), a, b, c), (a - b + c) & 0xFFFFFFFF)
            self.assertEqual(run(intops.iscadd(D, A, B, 5), a, b, c), ((a << 5) + b) & 0xFFFFFFFF)
            self.assertEqual(run(intops.iscadd(D, A, Imm(0x1235), 1), a, b, c), ((a << 1) + 0x1235) & 0xFFFFFFFF)
            self.assertEqual(run(intops.iscadd(D, A, B, 3, 'neg_a'), a, b, c), (b - (a << 3)) & 0xFFFFFFFF)

    def test_immediates_checked(self):
        with self.assertRaises(ValueError):
            intops.xmad(D, A, Imm(0x10000), C)
        with self.assertRaises(ValueError):
            intops.xmad(D, A, Imm(3), C, 'b_hi')
        with self.assertRaises(ValueError):
            intops.iscadd(D, A, B, 1, 'neg_a neg_b')


if __name__ == '__main__':
    unittest.main()
