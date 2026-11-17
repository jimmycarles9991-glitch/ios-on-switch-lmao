import math
import random
import struct
import unittest

from tools.benchloop import protocol
from tools.benchloop.sm53 import powr_native, powr_verdict, program
from tools.gpu.maxwell import floats, interp
from tools.gpu.maxwell.floats import F16, F32

# NAK's words for state 4c33d27bde9b894a's fast::powr (its fragment program, 14 Nov), with the registers they name
NAK_WORDS = {
    'widen': ((1, 1), 0x5CA8000000170601),
    'narrow': ((3, 9), 0x5CA8000000970903),
    'lg2': ((12, 6), 0x508000000037060C),
    'fmul': ((10, 10, 12), 0x5C68100000C70A0A),
    'rro': ((6, 10), 0x5C90008000A70006),
    'ex2': ((6, 6), 0x5080000000270606),
}
# metal2vulkan's src/lower/arith/tests/powr.rs HALF_SPECIALS
TRANSLATOR_SPECIALS = {
    0x0000, 0x8000, 0x0001, 0x8001, 0x03FF, 0x83FF, 0x0400, 0x3800, 0xB800, 0x3BFF, 0x3C00, 0x3C01,
    0xBC00, 0x4000, 0x7BFF, 0xFBFF, 0x7C00, 0xFC00, 0x7C01, 0x7E00, 0xFE00, 0x7FFF,
}  # fmt: skip


class Chip:
    """The interpreter's model of the four instructions, as fast host arithmetic (held to the interpreter below)."""

    def __init__(self) -> None:
        self.wide = [floats.convert(h, F16, F32) for h in range(1 << 16)]
        self.logs: dict[int, int] = {}

    def lg2(self, a: int) -> int:
        if a not in self.logs:
            self.logs[a] = interp.mufu(3, a)
        return self.logs[a]

    @staticmethod
    def ex2(a: int) -> int:
        return interp.mufu(2, a)

    @staticmethod
    def rro(a: int) -> int:
        return a

    @staticmethod
    def fmul(a: int, b: int) -> int:
        a, b = floats.flush(a, F32), floats.flush(b, F32)
        if floats.is_nan(a, F32) or floats.is_nan(b, F32):
            return F32.nan
        zero = [v & 0x7FFFFFFF == 0 for v in (a, b)]
        if any(floats.is_inf(v, F32) for v in (a, b)) and any(zero):
            return F32.nan
        return floats.flush(interp.to_f32(interp.from_f32(a) * interp.from_f32(b)), F32)

    @staticmethod
    def narrow(a: int) -> int:
        if floats.is_nan(a, F32):
            return F16.nan
        v = interp.from_f32(a)
        try:
            return struct.unpack('<H', struct.pack('<e', v))[0]
        except OverflowError:
            return F16.inf | (F16.sign_bit if v < 0 else 0)

    def chain(self, x: int, y: int) -> tuple[int, int]:
        r = self.ex2(self.rro(self.fmul(self.lg2(self.wide[x]), self.wide[y])))
        return r, self.narrow(r)

    def run(self, name: str, a: int, s: int) -> tuple[int, int]:
        if name == 'lg2':
            return self.lg2(a), self.ex2(self.rro(a))
        if name == 'narrow':
            return self.rro(a), self.narrow(a)
        if name == 'fmul':
            return self.fmul(a, s), self.fmul(s, a)
        return self.chain(a, s) if name == 'base' else self.chain(s, a)


class WrongLog(Chip):
    """A chip whose LG2 of a negative number is log2 of its magnitude."""

    def lg2(self, a: int) -> int:
        return super().lg2(a & 0x7FFFFFFF)


def fake_result(chip: Chip, x: dict) -> dict:
    name = x['meta']['program']
    words = [w for a, s in powr_native.inputs(x) for w in program.pack(*chip.run(name, a, s))]
    return {'status': 'ok', 'texels': {'data': protocol.b64(struct.pack(f'<{len(words)}I', *words))}}


class PowrNativeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.experiments = powr_native.generate()
        cls.chip = Chip()

    def test_one_valid_batch(self):
        protocol.batch('powr-native', self.experiments)
        self.assertEqual({x['meta']['program'] for x in self.experiments}, set(powr_native.LAYOUT))
        self.assertLessEqual(len(self.experiments), protocol.MAX_EXPERIMENTS)

    def test_instructions_are_the_states(self):
        for name, (regs, word) in NAK_WORDS.items():
            self.assertEqual(getattr(powr_native, name)(*regs).word, word, name)
        for name in ('base', 'exponent'):
            kinds = [i.name for i in powr_native.BODIES[name]() if i.name != 'NOP']
            self.assertEqual(kinds, ['F2F_reg', 'F2F_reg', 'MUFU', 'FMUL_reg', 'RRO_reg', 'MUFU', 'F2F_reg'], name)

    def test_inputs_cover_every_half_against_every_special(self):
        self.assertTrue(set(powr_native.CHAIN_TABLE) >= TRANSLATOR_SPECIALS)
        self.assertEqual(len(powr_native.CHAIN_TABLE), 32)
        for name in ('base', 'exponent'):
            pairs = {p for x in self.experiments if x['meta']['program'] == name for p in powr_native.inputs(x)}
            self.assertEqual(pairs, {(h, s) for h in range(1 << 16) for s in powr_native.CHAIN_TABLE}, name)
        unary = set(powr_native.unary_inputs())
        self.assertTrue(set(powr_native.every_half()) <= unary)
        for b in (0x00000001, 0x807FFFFF, 0x3F7FFFFF, 0x3F800001, 0xFF800000, *powr_native.NAN_PAYLOADS):
            self.assertIn(b, unary, hex(b))
        for name in ('lg2', 'narrow', 'fmul'):
            seen = {a for x in self.experiments if x['meta']['program'] == name for a, _ in powr_native.inputs(x)}
            self.assertEqual(seen, unary, name)

    def test_program_writes_what_the_interpreter_computes(self):
        rng = random.Random(4)
        for x in self.experiments:
            name = x['meta']['program']
            code, data = (protocol.unb64(x['program'][k]) for k in ('code', 'data'))
            width, height = x['target']['width'], x['target']['height']
            ins = powr_native.inputs(x)
            corners = [(0, 0), (width - 1, height - 1)]
            for px, py in corners + [(rng.randrange(width), rng.randrange(height)) for _ in range(5)]:
                got = program.unpack(program.expected_texel(code, data, (px, py)))
                self.assertEqual(got, self.chip.run(name, *ins[py * width + px]), (x['id'], px, py))

    def test_fast_model_is_the_interpreters(self):
        rng = random.Random(8)
        specials = [*powr_native.FMUL_TABLE, *powr_native.NAN_PAYLOADS, 0x3F7FFFFF, 0x7F7FFFFF, 0x00800000]
        values = specials + [rng.getrandbits(32) for _ in range(300)]
        for a in values:
            for b in specials + values[:20]:
                self.assertEqual(self.chip.fmul(a, b), floats.arith('mul', [a, b], F32, floats.RN, True), (a, b))
            self.assertEqual(self.chip.narrow(a), floats.convert(a, F32, F16), hex(a))

    def test_the_interpreters_chip_gives_fast_powr(self):
        results = {x['id']: fake_result(self.chip, x) for x in self.experiments}
        found = powr_verdict.analyse(self.experiments, results)
        self.assertEqual(found['missing'], [])
        for name, c in found['checks'].items():
            self.assertTrue(c['checked'] and not c['off'], (name, c))
        for name, r in found['chain'].items():
            self.assertEqual((r['pairs'], r['off'], r['off_bits'], r['off_f32']), (1 << 21, 0, 0, 0), name)
            self.assertGreater(r['special'], 1 << 19, name)
        self.assertTrue(found['half'] and found['half_bits'] and found['float'])
        self.assertEqual(found['nan_half'], F16.nan)
        verdict = powr_verdict.lines(found)[-1]
        self.assertIn('native chain is fast::powr yes, bit for bit after the narrowing yes', verdict)

    def test_a_wrong_log_is_named(self):
        some = [x for x in self.experiments if x['meta']['program'] in ('lg2', 'narrow') or x['id'] == 'powr.base.4']
        wrong = WrongLog()
        found = powr_verdict.analyse(some, {x['id']: fake_result(wrong, x) for x in some})
        off = {name for name, c in found['checks'].items() if c['off']}
        self.assertEqual(off, {'LG2(x < 0) = NaN [half]', 'LG2(x < 0) = NaN [float]'})
        self.assertGreater(found['chain']['base']['off'], 0)
        self.assertFalse(found['half'] or found['float'])
        self.assertIn('powr LG2(x < 0) = NaN [half] OFF', '\n'.join(powr_verdict.lines(found)))

    def test_rule_is_the_translators(self):
        w = powr_verdict.HALF
        self.assertEqual(w.fast_powr(0x0000, 0x0000), 0x7E00)
        self.assertEqual(w.fast_powr(0x7C00, 0x8000), 0x7E00)
        self.assertEqual(w.fast_powr(0x3C00, 0xFC00), 0x7E00)
        self.assertEqual(w.fast_powr(0x8001, 0x3C00), 0x7E00)
        self.assertEqual(w.fast_powr(0x8000, 0x3C00), 0)
        self.assertEqual(w.fast_powr(0x8000, 0xBC00), 0x7C00)
        self.assertEqual(w.fast_powr(0x3BFF, 0x7C00), 0)
        self.assertEqual(w.fast_powr(0x3C01, 0x7C00), 0x7C00)
        self.assertIsNone(w.fast_powr(0x7E00, 0x7C00))
        self.assertIsNone(w.fast_powr(0x4000, 0x3C00))
        self.assertTrue(powr_verdict.F32.is_zero(0x807FFFFF))
        self.assertTrue(math.isnan(interp.from_f32(F32.nan)))


if __name__ == '__main__':
    unittest.main()
