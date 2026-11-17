"""interp.py's arithmetic against the host's IEEE, and the gate's eye for a one-bit codegen change."""

import math
import random
import struct
import unittest
from pathlib import Path

from tools.gpu.maxwell import execute, floats, gate, isa, texture
from tools.gpu.maxwell.execute import alu, run
from tools.gpu.maxwell.interp import Lane, UnsupportedError

FIXTURE = Path(__file__).with_name('testdata') / 'image_draw_fragment.hex'
F2F_NEG = 1 << 45
ONE_TARGET_RGBA = (
    bytes(gate.OMAP_TARGET_BYTE) + struct.pack('<I', 0xF) + bytes(isa.HEADER_BYTES - gate.OMAP_TARGET_BYTE - 4)
)


def f32(x: float) -> int:
    try:
        return struct.unpack('<I', struct.pack('<f', x))[0]
    except OverflowError:
        return 0xFF800000 if x < 0 else 0x7F800000


def f16(x: float) -> int:
    try:
        return struct.unpack('<H', struct.pack('<e', x))[0]
    except OverflowError:
        return 0xFC00 if x < 0 else 0x7C00


def host(bits: int, fmt: str) -> float:
    return struct.unpack('<' + fmt, struct.pack('<' + ('I' if fmt == 'f' else 'H'), bits))[0]


class FloatsTest(unittest.TestCase):
    def test_f32_add_mul_round_like_the_host(self):
        rng = random.Random(1)
        for _ in range(5000):
            a, b = rng.getrandbits(32), rng.getrandbits(32)
            if floats.is_nan(a, floats.F32) or floats.is_nan(b, floats.F32):
                continue
            x, y = host(a, 'f'), host(b, 'f')
            for op, want in (('add', x + y), ('mul', x * y)):
                expect = floats.F32.nan if math.isnan(want) else f32(want)
                self.assertEqual(floats.arith(op, [a, b], floats.F32), expect, (op, hex(a), hex(b)))

    def test_f16_add_mul_and_narrowing(self):
        rng = random.Random(2)
        for _ in range(5000):
            a, b = rng.getrandbits(16), rng.getrandbits(16)
            if floats.is_nan(a, floats.F16) or floats.is_nan(b, floats.F16):
                continue
            x, y = host(a, 'e'), host(b, 'e')
            for op, want in (('add', x + y), ('mul', x * y)):
                expect = floats.F16.nan if math.isnan(want) else f16(want)
                self.assertEqual(floats.arith(op, [a, b], floats.F16), expect, (op, hex(a), hex(b)))
            c = rng.getrandbits(32)
            if not floats.is_nan(c, floats.F32):
                self.assertEqual(floats.convert(c, floats.F32, floats.F16), f16(host(c, 'f')), hex(c))

    def test_half_fma_rounds_once(self):
        a, b, c = 0x3C01, 0x3C01, 0x8000 | 0x3C02  # (1+u)^2 - (1+2u) = u^2: lost by two roundings
        self.assertNotEqual(floats.arith('fma', [a, b, c], floats.F16), 0)


class ImmediateTest(unittest.TestCase):
    def test_f2f_immediate_is_a_floats_top_bits(self):
        # F2F.F16.F32 r2, 1.0 and -2.0 (nv50_ir emitF2F: imm20 is the f32's top 20 bits, its sign at bit 56)
        for value, half in ((0x3F800000, 0x3C00), (0xC0000000, 0xC000)):
            w = 0x38A8 << 48 | (value >> 31) << 56 | (value >> 12 & 0x7FFFF) << 20 | 2 << 10 | 1 << 8 | 2
            lane = Lane()
            execute.alu(isa.name(w), w, lane, gate.Seeded(1, 0).inputs())
            self.assertEqual(lane.r(2), half)

    def test_fadd_saturates_where_32i_cannot(self):
        # FADD.SAT r2, r1, 2.0 (imm20) clamps to 1.0; FADD32I has no SAT bit, so NAK must not choose it
        w = 0x3858 << 48 | 1 << 50 | (0x40000000 >> 12 & 0x7FFFF) << 20 | 1 << 8 | 2
        lane = Lane()
        lane.w(1, 0x3F000000)
        execute.alu(isa.name(w), w, lane, gate.Seeded(1, 0).inputs())
        self.assertEqual(lane.r(2), 0x3F800000)


class IntegerFloatTest(unittest.TestCase):
    S64 = 0x5CB8000001872E0A

    def test_register_pair_i2f_rounds_the_signed_integer_once(self):
        cases = (
            (0, (0, 0, 0, 0)),
            (1, (0x3F800000,) * 4),
            (-1, (0xBF800000,) * 4),
            (2**24 + 1, (0x4B800000, 0x4B800000, 0x4B800001, 0x4B800000)),
            (-(2**24) - 1, (0xCB800000, 0xCB800001, 0xCB800000, 0xCB800000)),
            (2**32 + 1, (0x4F800000, 0x4F800000, 0x4F800001, 0x4F800000)),
            (-(2**32) - 1, (0xCF800000, 0xCF800001, 0xCF800000, 0xCF800000)),
            (2**62 + 2**38 + 1, (0x5E800001, 0x5E800000, 0x5E800001, 0x5E800000)),
            (-(2**62) - 2**38 - 1, (0xDE800001, 0xDE800001, 0xDE800000, 0xDE800000)),
            (-(2**63), (0xDF000000,) * 4),
            (2**63 - 1, (0x5F000000, 0x5EFFFFFF, 0x5F000000, 0x5EFFFFFF)),
        )
        for value, expected in cases:
            for mode, want in enumerate(expected):
                with self.subTest(value=value, mode=mode):
                    lane = Lane()
                    lane.w(24, value)
                    lane.w(25, value >> 32)
                    w = self.S64 | mode << 39
                    self.assertEqual(isa.name(w), 'I2F_reg')
                    alu(isa.name(w), w, lane, gate.Seeded(1, 0).inputs())
                    self.assertEqual(lane.r(10), want)

    def test_unimplemented_integer_widths_and_pair_modifiers_remain_unsupported(self):
        words = (
            self.S64 & ~(3 << 10),
            self.S64 & ~(3 << 10) | 1 << 10,
            self.S64 & ~(3 << 8) | 1 << 8,
            self.S64 | 1 << 41,
            self.S64 | 1 << 45,
            self.S64 | 1 << 49,
        )
        for w in words:
            with self.subTest(word=hex(w)), self.assertRaises(UnsupportedError):
                alu(isa.name(w), w, Lane(), gate.Seeded(1, 0).inputs())

    def test_zero_register_pair_and_unsigned_high_word(self):
        lane = Lane()
        zero = self.S64 & ~(0xFF << 20) | 0xFF << 20
        alu(isa.name(zero), zero, lane, gate.Seeded(1, 0).inputs())
        self.assertEqual(lane.r(10), 0)
        lane.w(24, 0xFFFFFFFF)
        lane.w(25, 0xFFFFFFFF)
        for mode, expected in enumerate((0x5F800000, 0x5F7FFFFF, 0x5F800000, 0x5F7FFFFF)):
            w = self.S64 & ~(1 << 13) | mode << 39
            alu(isa.name(w), w, lane, gate.Seeded(1, 0).inputs())
            self.assertEqual(lane.r(10), expected)


class ThreeSourceTest(unittest.TestCase):
    def run_one(self, w: int, regs: dict) -> int:
        lane = Lane()
        for r, v in regs.items():
            lane.w(r, v)
        execute.alu(isa.name(w), w, lane, gate.Seeded(1, 0).inputs())
        return lane.r(4)

    def test_lop3_iadd3_iscadd(self):
        regs = {1: 0x12345678, 2: 0x0F0F0F0F, 3: 0xFF00FF00}
        # LOP3.LUT r4, r1, r2, r3, 0x96 (a ^ b ^ c); with a 20-bit immediate b, 0xE8 (majority)
        lop3 = 0x5BE7 << 48 | 3 << 39 | 0x96 << 28 | 2 << 20 | 1 << 8 | 4
        self.assertEqual(self.run_one(lop3, regs), 0x12345678 ^ 0x0F0F0F0F ^ 0xFF00FF00)
        lop3i = 0x3C00 << 48 | 0xE8 << 48 | 3 << 39 | 0x7FFF << 20 | 1 << 8 | 4
        a, b, c = 0x12345678, 0x7FFF, 0xFF00FF00
        self.assertEqual(self.run_one(lop3i, regs), (a & b) | (a & c) | (b & c))
        # IADD3 r4, r1, -r2, r3
        iadd3 = 0x5CC0 << 48 | 1 << 50 | 3 << 39 | 2 << 20 | 1 << 8 | 4
        self.assertEqual(self.run_one(iadd3, regs), (0x12345678 - 0x0F0F0F0F + 0xFF00FF00) & 0xFFFFFFFF)
        # ISCADD r4, r1, 0x40, 3: (r1 << 3) + 0x40
        iscadd = 0x3818 << 48 | 3 << 39 | 0x40 << 20 | 1 << 8 | 4
        self.assertEqual(self.run_one(iscadd, regs), (0x12345678 << 3) + 0x40 & 0xFFFFFFFF)


class GateTest(unittest.TestCase):
    def setUp(self):
        self.code = isa.from_hex_dump(FIXTURE.read_text())

    def test_runs_to_exit(self):
        regs, killed = run(self.code, gate.Seeded(1, 0).inputs(), 4)
        self.assertEqual(len(regs), 4)
        self.assertFalse(killed)

    def test_identical_to_itself(self):
        header = ONE_TARGET_RGBA
        self.assertEqual(gate.compare((header + self.code, -1), (header + self.code, -1), 30, 1), 'identical')

    def test_sees_one_flipped_bit(self):
        words = list(struct.unpack(f'<{len(self.code) // 8}Q', self.code))
        at = max(i for i, w in enumerate(words) if i % 4 and isa.name(w) == 'F2F_reg')
        words[at] ^= F2F_NEG
        mutated = struct.pack(f'<{len(words)}Q', *words)
        header = ONE_TARGET_RGBA
        self.assertTrue(gate.compare((header + self.code, -1), (header + mutated, -1), 50, 1).startswith('different'))


class TextureTest(unittest.TestCase):
    # TEX_b r4, r0 (x, y), r2 (the handle) .2d rgba; TEXS r8:r10, r0 (x), r1 (y) .2d rgba at bound index 16
    TEX_B = 0xDE80_0000_0000_0000 | 0xF << 31 | 2 << 28 | 2 << 20 | 0 << 8 | 4
    TEXS = 0xD800_0000_0000_0000 | 1 << 53 | 4 << 50 | 16 << 36 | 10 << 28 | 1 << 20 | 0 << 8 | 8

    def lanes(self) -> tuple:
        inputs = gate.Seeded(3, 0).inputs(0, bound_cbuf=1)
        lane = Lane()
        lane.w(0, 0x3E800000)
        lane.w(1, 0x3F400000)
        lane.w(2, inputs.cbuf(1, 64))
        return inputs, lane

    def test_bound_scalar_and_bindless_forms_fetch_the_same_texel(self):
        inputs, lane = self.lanes()
        texture.tex('TEX_b', self.TEX_B, lane, inputs, 4)
        texture.texs(self.TEXS, lane, inputs)
        self.assertEqual([lane.r(r) for r in (4, 5, 6, 7)], [lane.r(r) for r in (8, 9, 10, 11)])

    # TLD r4, r0 (x, y) .2d rgba .lz at bound index 16; TLDS r8:r10, r0 (x), r1 (y) .2d .lz rgba at 16; TLD_b r12, r0,
    # r2 (the handle) .2d rgba .lz; TLD r4, r0, r3 (the level) .2d rgba .ll at 16
    TLD = 0xDC38_0000_0000_0000 | 16 << 36 | 0xF << 31 | 2 << 28 | 0xFF << 20 | 0 << 8 | 4
    TLDS = 0xD200_0000_0000_0000 | 1 << 59 | 2 << 53 | 4 << 50 | 16 << 36 | 10 << 28 | 1 << 20 | 0 << 8 | 8
    TLD_B = 0xDD38_0000_0000_0000 | 0xF << 31 | 2 << 28 | 2 << 20 | 0 << 8 | 12
    TLD_LL = 0xDC38_0000_0000_0000 | 1 << 55 | 16 << 36 | 0xF << 31 | 2 << 28 | 3 << 20 | 0 << 8 | 4

    def test_texel_loads_bound_scalar_and_bindless_read_the_same_texel(self):
        inputs, lane = self.lanes()
        lane.w(0, 7)
        lane.w(1, 9)
        self.assertEqual([isa.name(w) for w in (self.TLD, self.TLDS, self.TLD_B)], ['TLD', 'TLDS', 'TLD_b'])
        texture.tld('TLD', self.TLD, lane, inputs, 4)
        texture.tlds(self.TLDS, lane, inputs)
        texture.tld('TLD_b', self.TLD_B, lane, inputs, 12)
        loaded = [lane.r(r) for r in (4, 5, 6, 7)]
        self.assertEqual(loaded, [lane.r(r) for r in (8, 9, 10, 11)])
        self.assertEqual(loaded, [lane.r(r) for r in (12, 13, 14, 15)])

    def test_texel_load_is_no_sample_and_names_its_level(self):
        inputs, lane = self.lanes()
        texture.tld('TLD', self.TLD, lane, inputs, 4)
        loaded = [lane.r(r) for r in (4, 5, 6, 7)]
        texture.texs(self.TEXS, lane, inputs)
        self.assertNotEqual(loaded, [lane.r(r) for r in (8, 9, 10, 11)])
        lane.w(3, 1)
        texture.tld('TLD', self.TLD_LL, lane, inputs, 4)
        self.assertNotEqual(loaded, [lane.r(r) for r in (4, 5, 6, 7)])

    def test_texs_f16_packs_round_to_nearest_even_halves(self):
        inputs, lane = self.lanes()
        texture.tex('TEX_b', self.TEX_B, lane, inputs, 4)
        texture.texs(self.TEXS & ~(1 << 59), lane, inputs)
        want = [floats.convert(lane.r(r), floats.F32, floats.F16) for r in (4, 5, 6, 7)]
        self.assertEqual((lane.r(8), lane.r(10)), (want[0] | want[1] << 16, want[2] | want[3] << 16))

    # TLD_b r4, r0 (x, y), r2 (the handle) .2d rgba, level zero; TLDS r8:r10, r0 (x), r1 (y) .2d level zero at bound
    # index 16
    TLD_B_LZ = 0xDD38 << 48 | 0xF << 31 | 2 << 28 | 2 << 20 | 0 << 8 | 4
    TLDS_LZ = 0xD << 60 | 1 << 59 | 1 << 57 | 2 << 53 | 4 << 50 | 16 << 36 | 10 << 28 | 1 << 20 | 0 << 8 | 8

    def test_bound_scalar_and_bindless_loads_read_the_same_texel(self):
        inputs, lane = self.lanes()
        texture.tld('TLD_b', self.TLD_B_LZ, lane, inputs, 4)
        texture.tlds(self.TLDS_LZ, lane, inputs)
        self.assertEqual([lane.r(r) for r in (4, 5, 6, 7)], [lane.r(r) for r in (8, 9, 10, 11)])

    # TXD_b r4, r0 (the handle r0, x r1, y r2), r8 (dx/dy of x, of y) .2d rgba; TXD r12 the same at bound index 16
    TXD_B = 0xDE78 << 48 | 0xF << 31 | 2 << 28 | 8 << 20 | 0 << 8 | 4
    TXD = 0xDE38 << 48 | 16 << 36 | 0xF << 31 | 2 << 28 | 8 << 20 | 1 << 8 | 12

    def test_bound_and_bindless_derivative_fetches_read_the_same_texel(self):
        inputs, lane = self.lanes()
        lane.w(2, 0x3F400000)
        lane.w(1, 0x3E800000)
        lane.w(0, inputs.cbuf(1, 64))
        for r, v in zip(range(8, 12), (0x3C000000, 0, 0, 0x3C000000), strict=True):
            lane.w(r, v)
        texture.txd(isa.name(self.TXD_B), self.TXD_B, lane, inputs, 4)
        texture.txd(isa.name(self.TXD), self.TXD, lane, inputs, 12)
        self.assertEqual([lane.r(r) for r in (4, 5, 6, 7)], [lane.r(r) for r in (12, 13, 14, 15)])
        lane.w(9, 0x3C000001)
        texture.txd(isa.name(self.TXD), self.TXD, lane, inputs, 12)
        self.assertEqual([lane.r(r) for r in (4, 5, 6, 7)], [lane.r(r) for r in (12, 13, 14, 15)])

    def test_a_load_and_a_sample_of_one_texel_are_different_keys(self):
        inputs, lane = self.lanes()
        texture.tld('TLD_b', self.TLD_B_LZ, lane, inputs, 4)
        texture.tex('TEX_b', self.TEX_B, lane, inputs, 12)
        self.assertNotEqual([lane.r(r) for r in (4, 5, 6, 7)], [lane.r(r) for r in (12, 13, 14, 15)])

    # TLDS r8:r10, r4:r5 (x, y), r6 (level) .2d.ll rgba at bound index 17 (NAK's sm50/texs.rs tlds_and_tld4s)
    TLDS_LL = 0xDA00_0000_0000_0000 | 1 << 59 | 5 << 53 | 4 << 50 | 17 << 36 | 10 << 28 | 6 << 20 | 4 << 8 | 8

    def test_tlds_reads_the_words_its_target_names(self):
        inputs, lane = self.lanes()
        for r, v in ((4, 7), (5, 9), (6, 1)):
            lane.w(r, v)
        texture.tlds(self.TLDS_LL, lane, inputs)
        first = [lane.r(r) for r in (8, 9, 10, 11)]
        lane.w(7, 123)
        texture.tlds(self.TLDS_LL, lane, inputs)
        self.assertEqual([lane.r(r) for r in (8, 9, 10, 11)], first)
        lane.w(6, 2)
        texture.tlds(self.TLDS_LL, lane, inputs)
        self.assertNotEqual([lane.r(r) for r in (8, 9, 10, 11)], first)


class TextureGatherTest(unittest.TestCase):
    PLAIN = 0xDF000120C0270110
    OFFSET = 0xDF0A01D201F72422

    def test_f32_gather_uses_both_coordinates_and_writes_both_pairs(self):
        inputs = gate.Seeded(1, 0).inputs(bound_cbuf=2)
        cb_reads, keys = [], []
        inputs.cbuf = lambda bank, at: cb_reads.append((bank, at)) or 0x1234
        values = [f32(v) for v in (0.25, -0.5, 0.75, 1.0)]
        inputs.texture = lambda key: keys.append(key) or values
        lane = Lane()
        lane.w(1, f32(0.125))
        lane.w(2, f32(0.625))
        self.assertEqual(isa.name(self.PLAIN), 'TLD4S')
        alu('TLD4S', self.PLAIN, lane, inputs)
        self.assertEqual(cb_reads, [(2, 72)])
        self.assertEqual(keys, [('tld4', 0x1234, (f32(0.125), f32(0.625)), 0, (0, 0), texture.DIM_2D)])
        self.assertEqual([lane.r(r) for r in (16, 17, 12, 13)], values)

    def test_offset_gather_sign_extends_only_the_two_six_bit_offsets(self):
        inputs = gate.Seeded(1, 0).inputs(bound_cbuf=2)
        cb_reads, keys = [], []
        inputs.cbuf = lambda bank, at: cb_reads.append((bank, at)) or 0x5678
        values = [f32(v) for v in (0.25, -0.5, 0.75, 1.0)]
        inputs.texture = lambda key: keys.append(key) or values
        lane = Lane()
        lane.w(36, f32(0.125))
        lane.w(37, f32(0.625))
        self.assertEqual(isa.name(self.OFFSET), 'TLD4S')
        for packed, offset in ((0xFFFF, (-1, -1)), (0x01FF, (-1, 1)), (0xFF01, (1, -1)), (0x0101, (1, 1))):
            with self.subTest(offset=offset):
                lane.w(31, packed)
                alu('TLD4S', self.OFFSET, lane, inputs)
                self.assertEqual(keys[-1], ('tld4', 0x5678, (f32(0.125), f32(0.625)), 0, offset, texture.DIM_2D))
                self.assertEqual([lane.r(r) for r in (34, 35, 32, 33)], values)
                lane.w(31, packed ^ 0xFFFFC0C0)
                alu('TLD4S', self.OFFSET ^ 1 << 49, lane, inputs)
                self.assertEqual(keys[-1], keys[-2])
        self.assertEqual(set(cb_reads), {(2, 116)})

    def test_component_and_coordinate_changes_distinguish_gathers(self):
        inputs = gate.Seeded(1, 0).inputs(bound_cbuf=2)
        keys = []
        inputs.texture = lambda key: keys.append(key) or [0] * 4
        lane = Lane()
        lane.w(1, f32(0.125))
        lane.w(2, f32(0.625))
        texture.tld4s(self.PLAIN, lane, inputs)
        for component in (1, 2, 3):
            texture.tld4s(self.PLAIN | component << 52, lane, inputs)
            self.assertEqual(keys[-1][3], component)
            self.assertNotEqual(keys[-1], keys[0])
        lane.w(2, f32(0.75))
        texture.tld4s(self.PLAIN, lane, inputs)
        self.assertNotEqual(keys[-1], keys[0])
        zero_offset = self.PLAIN & ~(0xFF << 20) | 3 << 20 | 1 << 51
        texture.tld4s(zero_offset, lane, inputs)
        self.assertEqual(keys[-1], keys[-2])

    def test_f16_gather_packs_ties_to_even_and_signed_zero(self):
        inputs = gate.Seeded(1, 0).inputs(bound_cbuf=2)
        values = [f32(1 + 2**-11), f32(1 + 3 * 2**-11), f32(-0.0), f32(0.0)]
        inputs.texture = lambda _key: values
        lane = Lane()
        lane.w(17, 0x1234)
        lane.w(13, 0x5678)
        texture.tld4s(self.PLAIN | 1 << 55, lane, inputs)
        self.assertEqual((lane.r(16), lane.r(12)), (0x3C023C00, 0x00008000))
        self.assertEqual((lane.r(17), lane.r(13)), (0x1234, 0x5678))

    def test_discarded_pairs_and_zero_coordinate_register_do_not_spill(self):
        inputs = gate.Seeded(1, 0).inputs(bound_cbuf=2)
        keys = []
        inputs.texture = lambda key: keys.append(key) or [1, 2, 3, 4]
        lane = Lane()
        for half in (False, True):
            for discarded in (0, 28):
                word = self.PLAIN & ~(0xFF << discarded) | 0xFF << discarded | half << 55
                texture.tld4s(word, lane, inputs)
                self.assertEqual(lane.r(255), 0)
        word = self.OFFSET & ~(0xFF << 8) | 0xFF << 8
        texture.tld4s(word, lane, inputs)
        self.assertEqual(keys[-1][2], (0, 0))

    def test_depth_compare_remains_explicitly_unsupported(self):
        with self.assertRaisesRegex(UnsupportedError, 'TLD4S depth comparison'):
            texture.tld4s(self.PLAIN | 1 << 50, Lane(), gate.Seeded(1, 0).inputs())


class VertexAttributeTest(unittest.TestCase):
    # ALD.64 r4, a[0x80]; AST.64 a[0x90], r4; ALD.O r8, a[0x94] (the output AST wrote); RZ as offset and vertex
    ALD = 0xEFD8 << 48 | 1 << 47 | 0xFF << 39 | 0x80 << 20 | 0xFF << 8 | 4
    AST = 0xEFF0 << 48 | 1 << 47 | 0xFF << 39 | 1 << 32 | 0x90 << 20 | 0xFF << 8 | 4
    ALD_OUT = 0xEFD8 << 48 | 0xFF << 39 | 1 << 32 | 0x94 << 20 | 0xFF << 8 | 8

    def test_a_vertex_reads_its_inputs_and_its_own_outputs(self):
        inputs = gate.Seeded(1, 0).inputs()
        inputs.attribute = lambda at: 0x1000 + at
        lane = Lane()
        for w in (self.ALD, self.AST, self.ALD_OUT):
            self.assertIn(isa.name(w).split('_')[0], ('ALD', 'AST'))
            alu(isa.name(w), w, lane, inputs)
        self.assertEqual((lane.r(4), lane.r(5)), (0x1080, 0x1084))
        self.assertEqual([s for s in lane.stores if s[0] == 'ast'], [('ast', 0x90, 0x1080), ('ast', 0x94, 0x1084)])
        self.assertEqual(lane.r(8), 0x1084)


class InterpolationTest(unittest.TestCase):
    PASS_OFFSET_ZERO = 0xE023FF87CFF7FF00
    MUL_W_OFFSET_ZERO = 0xE063FF8D0007FF01
    PASS_OFFSET_R9 = 0xE02384880FF7FF09

    def test_zero_offset_pass_reads_the_center_attribute(self):
        inputs = gate.Seeded(1, 0).inputs()
        inputs.attribute = lambda at: f32(1.25) if at == 0x7C else 0xDEADBEEF
        lane = Lane()
        alu('IPA', self.PASS_OFFSET_ZERO, lane, inputs)
        self.assertEqual(lane.r(0), f32(1.25))

    def test_zero_offset_perspective_reads_the_center_and_multiplies_inv_w(self):
        inputs = gate.Seeded(1, 0).inputs()
        inputs.attribute = lambda at: f32(3.0) if at == 0xD0 else 0xDEADBEEF
        lane = Lane()
        lane.w(0, f32(0.5))
        alu('IPA', self.MUL_W_OFFSET_ZERO, lane, inputs)
        self.assertEqual(lane.r(1), f32(1.5))

    def test_dynamic_offset_is_a_signed_twelve_bit_pair_before_aliased_destination_write(self):
        inputs = gate.Seeded(1, 0).inputs()
        keys = []
        inputs.attribute = lambda key: keys.append(key) or f32(1.25)
        for packed in (0x0700F800, 0xA7009800):
            lane = Lane()
            lane.w(9, packed)
            alu('IPA', self.PASS_OFFSET_R9, lane, inputs)
            self.assertEqual(keys[-1], (0x80, -2048, 1792))
            self.assertEqual(lane.r(9), f32(1.25))

    def test_zero_offset_in_a_register_uses_the_same_center_input_as_rz(self):
        inputs = gate.Seeded(1, 0).inputs()
        keys = []
        inputs.attribute = lambda key: keys.append(key) or f32(2.0)
        lane = Lane()
        alu('IPA', self.PASS_OFFSET_R9, lane, inputs)
        self.assertEqual(keys, [0x80])
        self.assertEqual(lane.r(9), f32(2.0))

    def test_offset_identity_changes_the_seeded_input(self):
        values = []
        for packed in (1, 2, 0x10000, 0xFFFF):
            lane = Lane()
            lane.w(9, packed)
            alu('IPA', self.PASS_OFFSET_R9, lane, gate.Seeded(1, 0).inputs())
            values.append(lane.r(9))
        self.assertEqual(len(set(values)), 4)

    def test_other_index_address_state_and_nonzero_frequency_modes_remain_unmodelled(self):
        address = self.PASS_OFFSET_ZERO & ~(0xFF << 8) | 1 << 8
        state = self.PASS_OFFSET_ZERO | 3 << 54
        unknown_location = self.PASS_OFFSET_ZERO | 1 << 52
        for word in (
            address,
            state,
            unknown_location,
            self.PASS_OFFSET_ZERO | 1 << 38,
            self.PASS_OFFSET_R9 | 1 << 54,
            self.PASS_OFFSET_R9 | 2 << 54,
        ):
            with self.subTest(word=hex(word)), self.assertRaises(UnsupportedError):
                lane = Lane()
                lane.w(9, 1)
                alu('IPA', word, lane, gate.Seeded(1, 0).inputs())


class LocalTest(unittest.TestCase):
    # STL.64 [r2 - 8], r4; LDL.64 r8, [r2 - 8]; LDL r10, [r2 + 0x40] (never stored)
    STL = 0xEF50 << 48 | 5 << 48 | ((-8) & 0xFFFFFF) << 20 | 2 << 8 | 4
    LDL = 0xEF40 << 48 | 5 << 48 | ((-8) & 0xFFFFFF) << 20 | 2 << 8 | 8
    LDL_UNSTORED = 0xEF40 << 48 | 4 << 48 | 0x40 << 20 | 2 << 8 | 10

    def test_a_lane_reads_back_what_it_stored(self):
        inputs, lane = gate.Seeded(1, 0).inputs(), Lane()
        lane.w(2, 0x100)
        lane.w(4, 7)
        lane.w(5, 9)
        for w in (self.STL, self.LDL, self.LDL_UNSTORED):
            alu(isa.name(w), w, lane, inputs)
        self.assertEqual((lane.r(8), lane.r(9), lane.r(10)), (7, 9, 0))
        self.assertEqual(lane.local, {0xF8: 7, 0xFC: 9})


class AddressTest(unittest.TestCase):
    """The 64-bit address arithmetic and texel fetch m2v's argument-buffer reads compile to (NAK sm50.rs encodings)."""

    @staticmethod
    def shf(opcode: int, kind: int, shift: int, high: bool = False) -> int:
        return opcode << 48 | high << 48 | kind << 37 | shift << 20 | 1 << 39 | 0 << 8 | 2

    def test_shf_funnels_the_high_word_into_the_low(self):
        for opcode, kind, shift, high, want in (
            (0x38F8, 2, 4, False, 0x21234567),  # SHF.R.U64: (r1:r0) >> 4, its low word
            (0x38F8, 2, 4, True, 0x00BCDEF1),  # .HI
            (0x36F8, 2, 4, False, 0xBCDEF121),  # SHF.L.U64: (r1:r0) << 4, its high word
            (0x38F8, 3, 36, True, 0xFFFFFFFF),  # SHF.R.S64 .HI past 32: the sign
        ):
            lane = Lane()
            lane.w(0, 0x12345678)
            lane.w(1, 0xABCDEF12 if kind == 3 else 0xBCDEF12)
            alu('SHF_r_imm' if opcode == 0x38F8 else 'SHF_l_imm', self.shf(opcode, kind, shift, high), lane, None)
            self.assertEqual(lane.r(2), want, (hex(opcode), kind, shift, high))

    def test_a_64_bit_sum_carries_from_its_low_word_into_its_high(self):
        lane = Lane()
        for r, v in enumerate((0xFFF00040, 0xFFFFFFFF, 0x02100000, 0)):
            lane.w(r, v)
        # IADD.CC r4 = r0 + r2, then IADD.X r5 = r1 + r3 + carry: 0xFFFFFFFF_FFF00040 + 0x02100000 wraps to 0x2000040
        alu('IADD_reg', 0x5C10 << 48 | 1 << 47 | 2 << 20 | 0 << 8 | 4, lane, None)
        alu('IADD_reg', 0x5C10 << 48 | 1 << 43 | 3 << 20 | 1 << 8 | 5, lane, None)
        self.assertEqual((lane.r(4), lane.r(5)), (0x02000040, 0))
        # a subtraction carries where it does not borrow: 5 - 5 with the second operand negated
        lane.w(6, 5)
        alu('IADD_reg', 0x5C10 << 48 | 1 << 47 | 1 << 48 | 6 << 20 | 6 << 8 | 7, lane, None)
        self.assertEqual((lane.r(7), lane.carry), (0, 1))

    def test_a_bindless_texel_fetch_takes_its_handle_from_register_b(self):
        seen = []
        inputs = gate.Seeded(3, 0).inputs(0)
        inputs.texture = lambda key: seen.append(key) or [1, 2, 3, 4]
        lane = Lane()
        lane.w(4, 7)
        lane.w(5, 9)
        lane.w(8, 0x300005)
        # TLD.B 2D (dim 1), RGBA (mask 15), level zero: coordinates r4, r5, handle r8, results from r10
        word = 0xDD38 << 48 | 15 << 31 | 1 << 28 | 8 << 20 | 4 << 8 | 10
        texture.tld('TLD_b', word, lane, inputs, 10)
        self.assertEqual(seen, [('tld', 0x300005, (7, 9), None, None, None, 1)])
        self.assertEqual([lane.r(r) for r in (10, 11, 12, 13)], [1, 2, 3, 4])


class AttributeTest(unittest.TestCase):
    """A vertex stage's ALD and AST as NAK sm50.rs encodes them: 0xefd8 and 0xeff0, the byte address in bits 20-29."""

    @staticmethod
    def word(opcode: int, reg: int, index: int, address: int, comps: int) -> int:
        output = 1 << 32 if opcode == 0xEFF0 else 0
        return opcode << 48 | (comps - 1) << 47 | 0xFF << 39 | output | address << 20 | index << 8 | reg

    def test_a_load_reads_consecutive_words_from_its_address_plus_its_index(self):
        inputs = gate.Seeded(3, 0).inputs(0)
        inputs.attribute = lambda at: 0x1000 + at
        lane = Lane()
        lane.w(2, 0x10)
        alu('ALD', self.word(0xEFD8, 4, 255, 0x80, 4), lane, inputs)
        self.assertEqual([lane.r(r) for r in range(4, 8)], [0x1080, 0x1084, 0x1088, 0x108C])
        alu('ALD', self.word(0xEFD8, 8, 2, 0x2F0, 1), lane, inputs)
        self.assertEqual(lane.r(8), 0x1300)

    def test_a_store_records_each_word_at_its_address(self):
        lane = Lane()
        for r, v in enumerate((5, 6, 7, 8)):
            lane.w(10 + r, v)
        alu('AST', self.word(0xEFF0, 10, 255, 0x70, 4), lane, None)
        self.assertEqual(lane.stores, [('ast', 0x70, 5), ('ast', 0x74, 6), ('ast', 0x78, 7), ('ast', 0x7C, 8)])

    def test_a_patch_or_a_vertex_register_is_not_modelled(self):
        with self.assertRaises(UnsupportedError):
            alu('ALD', self.word(0xEFD8, 4, 255, 0x80, 1) | 1 << 31, Lane(), None)


if __name__ == '__main__':
    unittest.main()
