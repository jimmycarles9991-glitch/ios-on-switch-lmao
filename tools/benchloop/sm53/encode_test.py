"""encode.py against isa.py's patterns, the interpreter's arithmetic and the words NAK wrote for a real program."""

import math
import struct
import unittest
from pathlib import Path

from tools.benchloop.sm53 import encode as e, program
from tools.benchloop.sm53.encode import PT, Cbuf, Control, Imm, Pred, f32, half2
from tools.gpu.maxwell import isa
from tools.gpu.maxwell.isa import field as bits

FIXTURE = Path(__file__).parents[3] / 'tools/gpu/maxwell/testdata/image_draw_fragment.hex'
CONSTRUCTED = [
    (e.mov(1, 2), 'MOV_reg'),
    (e.mov(1, Cbuf(1, 8)), 'MOV_cbuf'),
    (e.mov32i(1, 5), 'MOV32I'),
    (e.fadd(1, 2, 3), 'FADD_reg'),
    (e.fadd(1, 2, Imm(f32(1.0))), 'FADD_imm'),
    (e.fadd(1, 2, Cbuf(1, 4)), 'FADD_cbuf'),
    (e.fmul(1, 2, 3), 'FMUL_reg'),
    (e.fmul(1, 2, Imm(f32(2.0))), 'FMUL_imm'),
    (e.fmul(1, 2, Cbuf(1, 4)), 'FMUL_cbuf'),
    (e.ffma(1, 2, 3, 4), 'FFMA_reg'),
    (e.ffma(1, 2, Imm(f32(0.5)), 4), 'FFMA_imm'),
    (e.ffma(1, 2, Cbuf(1, 0), 4), 'FFMA_cr'),
    (e.ffma(1, 2, 3, Cbuf(1, 0)), 'FFMA_rc'),
    (e.fmnmx(1, 2, 3, 'min'), 'FMNMX_reg'),
    (e.fsetp(0, 2, 3, 'lt'), 'FSETP_reg'),
    (e.fsetp(0, 2, Imm(0), 'lt'), 'FSETP_imm'),
    (e.hadd2(1, 2, 3), 'HADD2_reg'),
    (e.hmul2(1, 2, 3), 'HMUL2_reg'),
    (e.hfma2(1, 2, 3, 4), 'HFMA2_reg'),
    (e.mufu(1, 2, 'rcp'), 'MUFU'),
    (e.rro(1, 2), 'RRO_reg'),
    (e.f2f(1, 2), 'F2F_reg'),
    (e.f2i(1, 2), 'F2I_reg'),
    (e.i2f(1, 2), 'I2F_reg'),
    (e.iadd(1, 2, 3), 'IADD_reg'),
    (e.iadd(1, 2, Imm(-5)), 'IADD_imm'),
    (e.iadd(1, 2, Cbuf(1, 0)), 'IADD_cbuf'),
    (e.iadd32i(1, 2, 0x12345678), 'IADD32I'),
    (e.imul(1, 2, 3), 'IMUL_reg'),
    (e.lop(1, 2, 3, 'xor'), 'LOP_reg'),
    (e.lop(1, 2, Imm(-4), 'and'), 'LOP_imm'),
    (e.lop32i(1, 2, 0xFF00FF00, 'or'), 'LOP32I'),
    (e.shl(1, 2, Imm(3)), 'SHL_imm'),
    (e.shr(1, 2, 3), 'SHR_reg'),
    (e.imnmx(1, 2, 3, 'max'), 'IMNMX_reg'),
    (e.bfe(1, 2, Imm(0x0804)), 'BFE_imm'),
    (e.flo(1, 2), 'FLO_reg'),
    (e.sel(1, 2, 3, 1), 'SEL_reg'),
    (e.prmt(1, 2, Imm(0x3210), 3), 'PRMT_imm'),
    (e.prmt(1, 2, 4, 3), 'PRMT_reg'),
    (e.isetp(0, 2, 3, 'lt s32'), 'ISETP_reg'),
    (e.isetp(0, 2, Imm(7), 'gt'), 'ISETP_imm'),
    (e.ipa(1, 0x70), 'IPA'),
    (e.nop(), 'NOP'),
    (e.exit_(), 'EXIT'),
]


def _b(w: int, name: str, imm: str = 'i20') -> int | Cbuf | Imm:
    if name.endswith('_reg'):
        return bits(w, 20, 8)
    if name.endswith(('_cbuf', '_cr')):
        return Cbuf(bits(w, 34, 5), bits(w, 20, 14) * 4)
    v = bits(w, 20, 19) | bits(w, 56, 1) << 19
    if imm == 'f20':
        return Imm(v << 12)
    return Imm(v | 0xFFF00000 if v & 0x80000 else v)


def _pred(w: int, at: int) -> Pred:
    return Pred(bits(w, at, 3), bool(bits(w, at + 3, 1)))


def _name_of(table: dict[str, int], value: int) -> str:
    return next(k for k, v in table.items() if v == value)


def _flags(w: int, named: dict[str, int]) -> list[str]:
    return [name for name, at in named.items() if bits(w, at, 1)]


def _f2f(w: int, n: str) -> e.Instr:
    widths = {1: 'f16', 2: 'f32'}
    m = [f'dst={widths[bits(w, 8, 2)]}', f'src={widths[bits(w, 10, 2)]}', _name_of(e.ROUND, bits(w, 39, 2))]
    m += _flags(w, {'high': 41, 'round': 42, 'ftz': 44, 'neg_b': 45, 'abs_b': 49})
    return e.f2f(bits(w, 0, 8), _b(w, n), ' '.join(m))


def _hadd2(w: int, _: str) -> e.Instr:
    swz = {v: k for k, v in e.SWIZZLE.items()}
    m = [f'a={swz[bits(w, 47, 2)]}', f'b={swz[bits(w, 28, 2)]}']
    m += _flags(w, {'abs_b': 30, 'neg_b': 31, 'sat': 32, 'ftz': 39, 'neg_a': 43, 'abs_a': 44})
    m += ['out=f32'] if bits(w, 49, 2) == 1 else []
    return e.hadd2(bits(w, 0, 8), bits(w, 8, 8), bits(w, 20, 8), ' '.join(m))


def _isetp(w: int, n: str) -> e.Instr:
    test = [_name_of(e.INT_CMP, bits(w, 49, 3)), _name_of(e.COMBINE, bits(w, 45, 2))]
    test += ['s32'] if bits(w, 48, 1) else []
    return e.isetp(bits(w, 3, 3), bits(w, 8, 8), _b(w, n), ' '.join(test), _pred(w, 39))


def _lop(w: int, n: str) -> e.Instr:
    op = _name_of(e.LOGIC, bits(w, 41, 2))
    return e.lop(bits(w, 0, 8), bits(w, 8, 8), _b(w, n), op, ' '.join(_flags(w, {'inv_a': 39, 'inv_b': 40})))


def _lop32i(w: int, _: str) -> e.Instr:
    op = _name_of(e.LOGIC, bits(w, 53, 2))
    return e.lop32i(bits(w, 0, 8), bits(w, 8, 8), bits(w, 20, 32), op, ' '.join(_flags(w, {'inv_a': 55, 'inv_b': 56})))


def _ipa(w: int, _: str) -> e.Instr:
    m = [_name_of(e.FREQ, bits(w, 54, 2)), _name_of(e.LOCATION, bits(w, 52, 2)), *_flags(w, {'sat': 51})]
    return e.ipa(bits(w, 0, 8), bits(w, 28, 10), ' '.join(m), bits(w, 20, 8))


# The fixture's opcodes in encode.py's subset: their operands decoded, re-encoded through the public constructors
REENCODE = {
    'MOV_reg': lambda w, n: e.mov(bits(w, 0, 8), _b(w, n)),
    'MOV_cbuf': lambda w, n: e.mov(bits(w, 0, 8), _b(w, n)),
    'MOV32I': lambda w, _: e.mov32i(bits(w, 0, 8), bits(w, 20, 32)),
    'F2F_reg': _f2f,
    'SEL_reg': lambda w, n: e.sel(bits(w, 0, 8), bits(w, 8, 8), _b(w, n), _pred(w, 39)),
    'LOP_imm': _lop,
    'LOP_reg': _lop,
    'LOP32I': _lop32i,
    'ISETP_reg': _isetp,
    'ISETP_imm': _isetp,
    'HADD2_reg': _hadd2,
    'PRMT_imm': lambda w, n: e.prmt(bits(w, 0, 8), bits(w, 8, 8), _b(w, n), bits(w, 39, 8)),
    'IPA': _ipa,
    'NOP': lambda _w, _n: e.nop(),
    'EXIT': lambda _w, _n: e.exit_(),
}


def _run(body: list[e.Instr], init: dict[int, int], result: int = 8) -> int:
    prog = program.fragment(body, init, (result, result))
    return program.unpack(program.expected_texel(prog.code, prog.data))[0]


def _float(word: int) -> float:
    return struct.unpack('<f', struct.pack('<I', word))[0]


class EncodeTest(unittest.TestCase):
    def test_every_constructor_decodes_to_its_opcode(self):
        for instr, name in CONSTRUCTED:
            self.assertEqual(instr.name, name, hex(instr.word))

    def test_a_wait_one_cycle_after_its_barrier_is_set_is_refused(self):
        ipa = e.ipa(4, 0x70).sched(wr_bar=0)
        consumer = e.mov(0, 4).sched(wait=1)
        self.assertEqual(e.barrier_hazards([ipa, consumer]), [1])
        self.assertEqual(e.barrier_hazards([ipa.sched(delay=e.BARRIER_SET_CYCLES), consumer]), [])
        with self.assertRaises(ValueError):
            e.bundle([ipa, consumer])

    def test_control_bits_round_trip(self):
        c = Control(delay=13, yld=True, wr_bar=2, rd_bar=4, wait=0x21, reuse=0b101)
        self.assertEqual(Control.of(c.bits()), c)
        self.assertEqual(Control().bits(), 1 | 7 << 5 | 7 << 8)

    def test_bundle_pads_and_round_trips(self):
        instrs = [e.fadd(8, 8, 5).sched(delay=d) for d in (3, 4, 5, 6)]
        code = e.bundle(instrs)
        self.assertEqual(len(code), 64)
        back = e.unbundle(code)
        self.assertEqual(back[:4], instrs)
        self.assertEqual([i.name for i in back[4:]], ['NOP', 'NOP'])

    def test_rejects_what_does_not_fit(self):
        with self.assertRaises(ValueError):
            e.fadd(1, 2, Imm(f32(0.1)))
        with self.assertRaises(ValueError):
            e.iadd(1, 2, Imm(1 << 20))
        with self.assertRaises(ValueError):
            e.fadd(1, 2, 3, 'neg_c')

    def test_float_arithmetic_matches_the_host(self):
        x, y, z = 1.5, -2.25, 0.375
        init = {9: f32(x), 10: f32(y), 11: f32(z)}
        self.assertEqual(_float(_run([e.fadd(8, 9, 10)], init)), x + y)
        self.assertEqual(_float(_run([e.fmul(8, 9, 10)], init)), x * y)
        self.assertEqual(_float(_run([e.ffma(8, 9, 10, 11)], init)), x * y + z)
        self.assertEqual(_float(_run([e.ffma(8, 9, Imm(f32(2.0)), 11, 'neg_c')], init)), x * 2 - z)
        self.assertEqual(_float(_run([e.fmnmx(8, 9, 10, 'max', 'neg_b')], init)), max(x, -y))
        self.assertEqual(_float(_run([e.mufu(8, 9, 'rcp')], init)), struct.unpack('<f', struct.pack('<f', 1 / x))[0])
        self.assertEqual(_float(_run([e.f2f(8, 9, 'round rm')], init)), math.floor(x))
        self.assertEqual(_run([e.f2i(8, 10, 'rz')], init), -2 & 0xFFFFFFFF)
        self.assertEqual(_float(_run([e.i2f(8, 12)], {12: -7 & 0xFFFFFFFF})), -7.0)

    def test_half_arithmetic_matches_the_host(self):
        init = {9: half2(1.5, -2.0), 10: half2(0.25, 3.0), 11: half2(1.0, 1.0)}
        want = {
            'HADD2': (1.75, 1.0),
            'HMUL2': (0.375, -6.0),
            'HFMA2': (1.375, -5.0),
        }
        got = {
            'HADD2': _run([e.hadd2(8, 9, 10)], init),
            'HMUL2': _run([e.hmul2(8, 9, 10)], init),
            'HFMA2': _run([e.hfma2(8, 9, 10, 11)], init),
        }
        for op, (lo, hi) in want.items():
            self.assertEqual(struct.unpack('<ee', struct.pack('<I', got[op])), (lo, hi), op)

    def test_integer_arithmetic_matches_the_host(self):
        a, b = 0x89ABCDEF, 0x12345678
        init = {9: a, 10: b}
        m = 0xFFFFFFFF
        cases = [
            (e.iadd(8, 9, 10), (a + b) & m),
            (e.iadd(8, 9, 10, 'neg_b'), (a - b) & m),
            (e.iadd32i(8, 9, 0x100), (a + 0x100) & m),
            (e.imul(8, 9, 10), (a * b) & m),
            (e.imul(8, 9, 10, 'hi'), (a * b) >> 32),
            (e.lop(8, 9, 10, 'xor'), a ^ b),
            (e.lop(8, 9, 10, 'and', 'inv_b'), a & ~b & m),
            (e.lop32i(8, 9, 0xFFFF, 'or'), a | 0xFFFF),
            (e.shl(8, 9, Imm(4)), (a << 4) & m),
            (e.shr(8, 9, Imm(4)), a >> 4),
            (e.shr(8, 9, Imm(4), 's32'), ((a - (1 << 32)) >> 4) & m),
            (e.imnmx(8, 9, 10, 'min', 's32'), a),
            (e.imnmx(8, 9, 10, 'min'), b),
            (e.bfe(8, 9, Imm(0x0804)), (a >> 4) & 0xFF),
            (e.flo(8, 10), 28),
            (e.sel(8, 9, 10, Pred(PT, neg=True)), b),
            (e.prmt(8, 9, Imm(0x0321)), (a >> 8 | a << 24) & m),
            (e.mov32i(8, 0xCAFE), 0xCAFE),
            (e.mov(8, Cbuf(1, 4)), b),
        ]
        for instr, want in cases:
            self.assertEqual(_run([instr], init), want, instr.name)

    def test_predicates_and_attributes(self):
        init = {9: 3, 10: 5, 11: f32(1.0), 12: f32(-1.0), 13: 0xAAAA, 14: 0x5555}
        body = [e.isetp(1, 9, 10, 'lt s32').sched(delay=13), e.sel(8, 13, 14, 1)]
        self.assertEqual(_run(body, init), 0xAAAA)
        body = [e.fsetp(2, 11, 12, 'lt').sched(delay=13), e.sel(8, 13, 14, 2)]
        self.assertEqual(_run(body, init), 0x5555)
        prog = program.fragment([e.ipa(8, 0x70), e.ipa(9, 0x74)], {}, (8, 9))
        x, y = program.unpack(program.expected_texel(prog.code, prog.data, (3, 5)))
        self.assertEqual((_float(x), _float(y)), (3.5, 5.5))

    def test_nak_fixture_re_encodes_bit_for_bit(self):
        code = isa.from_hex_dump(FIXTURE.read_text())
        words = struct.unpack(f'<{len(code) // 8}Q', code)
        covered = set()
        for i, w in enumerate(words):
            if i % 4 == 0:
                controls = [Control.of(w >> (21 * k) & 0x1FFFFF) for k in range(3)]
                self.assertEqual(sum(c.bits() << (21 * k) for k, c in enumerate(controls)), w, f'scheduling word {i}')
                continue
            name = isa.name(w)
            if name in REENCODE:
                self.assertEqual(hex(REENCODE[name](w, name).word), hex(w), f'{name} at word {i}')
                covered.add(name)
        self.assertEqual(covered, set(REENCODE) - {'LOP_reg'})


if __name__ == '__main__':
    unittest.main()
