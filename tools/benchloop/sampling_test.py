"""The sampling programs on the host interpreter: the texel at the pixel's centre, and the rounded box average."""

import random
import struct
import unittest

from tools.benchloop import protocol, sampling
from tools.benchloop.sm53 import encode
from tools.gpu.maxwell import isa
from tools.gpu.maxwell.execute import run
from tools.gpu.maxwell.interp import Inputs

POSITION = (0x70, 0x74)


def f32(word: int) -> float:
    return struct.unpack('<f', struct.pack('<I', word))[0]


def environment(program: dict, x: int, y: int, texel: object) -> Inputs:
    data = protocol.unb64(program['data'])

    def cbuf(bank: int, offset: int) -> int:
        return struct.unpack_from('<I', data, offset)[0] if bank == 1 and offset + 4 <= len(data) else 0

    def attribute(addr: int) -> int:
        return encode.f32((x, y)[POSITION.index(addr)] + 0.5) if addr in POSITION else 0

    def texture(key: tuple) -> list[int]:
        _, _, coords, mode, *_ = key
        assert mode == sampling.LOD_ZERO
        return texel(f32(coords[0]), f32(coords[1]))

    return Inputs(attribute, cbuf, texture)


class SamplingTest(unittest.TestCase):
    def test_fetches_are_bound_to_the_placeholder_at_level_zero(self) -> None:
        words = isa.instructions(protocol.unb64(sampling.average4(64, 64)['code']))
        fetches = [w for w in words if isa.name(w) == 'TEX']
        self.assertEqual(len(fetches), 4)
        self.assertTrue(
            all(w >> 36 & 0x1FFF == sampling.PLACEHOLDER and w >> 55 & 3 == sampling.LOD_ZERO for w in fetches)
        )

    def test_scale_samples_at_the_pixel_centre(self) -> None:
        p = sampling.scale(32, 16)
        seen = []

        def texel(u: float, v: float) -> list[int]:
            seen.append((u, v))
            return [encode.f32(0.25)] * 4

        regs, _ = run(protocol.unb64(p['code']), environment(p, 3, 5, texel), 4)
        self.assertEqual(seen, [(3.5 / 32, 5.5 / 16)])
        self.assertEqual(regs, [encode.f32(0.25)] * 4)

    def test_average4_is_the_rounded_box_mean_of_the_codes(self) -> None:
        rng = random.Random(3)
        side = 8
        codes = [[[rng.randrange(256) for _ in range(4)] for _ in range(2 * side)] for _ in range(2 * side)]
        p = sampling.average4(2 * side, 2 * side)

        def texel(u: float, v: float) -> list[int]:
            tx, ty = int(u * 2 * side), int(v * 2 * side)
            self.assertAlmostEqual(u * 2 * side, tx + 0.5, places=4)
            return [encode.f32(c / 255) for c in codes[ty][tx]]

        for y in range(side):
            for x in range(side):
                regs, _ = run(protocol.unb64(p['code']), environment(p, x, y, texel), 4)
                for ch in range(4):
                    box = [codes[2 * y + j][2 * x + i][ch] for j in (0, 1) for i in (0, 1)]
                    self.assertEqual(round(f32(regs[ch]) * 255), (sum(box) + 2) >> 2, (x, y, ch))


if __name__ == '__main__':
    unittest.main()
