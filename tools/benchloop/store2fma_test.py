import random
import struct
import unittest

from tools.benchloop import protocol, rop, store2fma, texels
from tools.benchloop.sm53 import program


def fake_texel(fmt: str, codes: list[int]) -> bytes:
    if fmt == 'rgba16float':
        return struct.pack('<4H', *codes)
    if fmt == 'r16unorm':
        return struct.pack('<H', codes[0])
    if fmt == 'rgba16unorm':
        return struct.pack('<4H', *codes)
    colour = [c / ((1 << n) - 1) for c, n in zip(codes, texels.UNORM_BITS[fmt], strict=True)]
    return texels.encode(tuple(colour), fmt)


def fake_result(x: dict, flip: int | None = None) -> dict:
    fmt = x['target']['format']
    tops, lanes, _ = store2fma.FORMATS[fmt]
    out = []
    for i, words in enumerate(store2fma.inputs(x)):
        codes = [store2fma.predicted(words[k], tops[k]) for k in range(lanes)] + [0] * (4 - lanes)
        if i == flip:
            codes[0] ^= 1
        out.append(fake_texel(fmt, codes))
    return {'status': 'ok', 'texels': {'data': protocol.b64(b''.join(out))}}


class Store2FmaTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.experiments = store2fma.generate()

    def test_one_valid_batch(self):
        protocol.batch('store2fma', self.experiments)
        self.assertEqual({x['target']['format'] for x in self.experiments}, set(store2fma.FORMATS))

    def test_inputs_cover_every_tie_and_code_step(self):
        x = next(x for x in self.experiments if x['id'] == 'store2fma.bgra8unorm.ties.0')
        lane0 = {words[0] for words in store2fma.inputs(x)}
        for k in range(255):
            tie = texels.f32((2 * k + 1) / 510)
            for ulps in range(-6, 7):
                self.assertIn(store2fma.bits_of(rop.nudged(tie, ulps)) & 0xFFFFFFFF, lane0 | _other_lanes(x))

    def test_rgb10a2_alpha_takes_its_own_ties_and_codes(self):
        for sweep, steps, values in (
            ('ties', range(-6, 7), (1 / 6, 1 / 2, 5 / 6)),
            ('codes', range(-2, 3), (1 / 3, 2 / 3)),
        ):
            x = next(x for x in self.experiments if x['id'] == f'store2fma.rgb10a2unorm.{sweep}.0')
            alpha = {words[3] for words in store2fma.inputs(x)}
            for v in values:
                for ulps in steps:
                    self.assertIn(store2fma.bits_of(rop.nudged(texels.f32(v), ulps)), alpha, (sweep, v, ulps))

    def test_thinned_formats_keep_every_special(self):
        xs = [x for x in self.experiments if x['id'].startswith('store2fma.r16unorm.list')]
        seen = {words[0] for x in xs for words in store2fma.inputs(x)}
        self.assertTrue(set(store2fma.SPECIAL_BITS) <= seen)

    def test_every_half_and_special_is_an_input(self):
        xs = [x for x in self.experiments if x['id'].startswith('store2fma.rgba16unorm.list')]
        seen = {w for x in xs for words in store2fma.inputs(x) for w in words}
        self.assertTrue(set(store2fma.every_half()) <= seen)
        self.assertTrue(set(store2fma.SPECIAL_BITS) <= seen)

    def test_program_writes_the_rounded_values(self):
        rng = random.Random(9)
        for x in self.experiments:
            tops = store2fma.FORMATS[x['target']['format']][0]
            code, data = (protocol.unb64(x['program'][k]) for k in ('code', 'data'))
            width, height = x['target']['width'], x['target']['height']
            inputs = store2fma.inputs(x)
            for _ in range(6):
                px, py = rng.randrange(width), rng.randrange(height)
                words = inputs[py * width + px]
                got = program.expected_texel(code, data, (px, py))
                for lane in range(4):
                    v = store2fma.float_of(words[lane])
                    want = words[lane] if tops[lane] == 0 else store2fma.bits_of(store2fma.rounded(v, tops[lane]))
                    self.assertTrue(got[lane] == want or _both_nan(got[lane], want), (x['id'], px, py, lane))

    def test_two_ffmas_give_metals_code_for_8_and_10_bits_and_halves(self):
        for x in self.experiments:
            fmt = x['target']['format']
            if fmt not in ('bgra8unorm', 'rgb10a2unorm', 'rgba16float'):
                continue
            tops, lanes, _ = store2fma.FORMATS[fmt]
            for words in store2fma.inputs(x)[::3]:
                for lane in range(lanes):
                    key = (words[lane], tops[lane])
                    self.assertEqual(store2fma.predicted(*key), store2fma.wanted(*key), (x['id'], hex(key[0])))

    def test_two_ffmas_give_metals_code_for_16_bits(self):
        rng = random.Random(16)
        x = next(x for x in self.experiments if x['id'] == 'store2fma.rgba16unorm.ties.0')
        for words in rng.sample(store2fma.inputs(x), 4000):
            for w in words:
                self.assertEqual(store2fma.predicted(w, 65535), store2fma.wanted(w, 65535), hex(w))

    def test_analysis(self):
        some = [x for x in self.experiments if x['id'].endswith('.ties.0') or x['id'].endswith('mids.0')]
        results = {x['id']: fake_result(x) for x in some}
        results[some[0]['id']] = fake_result(some[0], flip=5)
        found = store2fma.analyse(some, results)
        self.assertEqual(found['bgra8unorm']['off_metal'], 1)
        self.assertEqual(found['bgra8unorm']['off_predicted'], 1)
        self.assertEqual(found['rgb10a2unorm']['off_metal'], 0)
        self.assertEqual(found['rgba16float']['off_metal'], 0)
        self.assertGreater(found['rgba16unorm']['checked'], 4 * 4096)

    def test_round_f32(self):
        self.assertEqual(store2fma.round_f32(store2fma.Fraction(1, 3)), texels.f32(1 / 3))
        self.assertEqual(store2fma.round_f32(store2fma.Fraction(2**128)), float('inf'))
        self.assertEqual(store2fma.round_f32(store2fma.Fraction(1, 2**150)), 0.0)
        self.assertEqual(store2fma.round_f32(store2fma.Fraction(3, 2**150)), 2.0**-149 * 2)


def _other_lanes(x: dict) -> set[int]:
    return {w for words in store2fma.inputs(x) for w in words[1:]}


def _both_nan(a: int, b: int) -> bool:
    return a & 0x7F800000 == 0x7F800000 and a & 0x7FFFFF != 0 and b & 0x7F800000 == 0x7F800000 and b & 0x7FFFFF != 0


if __name__ == '__main__':
    unittest.main()
