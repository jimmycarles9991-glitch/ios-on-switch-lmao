"""The chip model gives the device's first misses of gpu.x1.blend_exact (boots of 11 Nov, raw and NVK alike)."""

import unittest

from tools.benchloop import blend_models as bm

PAIRS = {
    'sov': ('one', 'one_minus_src_alpha'),
    'dov': ('one_minus_dst_alpha', 'one'),
    'sin': ('dst_alpha', 'zero'),
    'din': ('zero', 'src_alpha'),
    'add': ('one', 'one'),
}
CASES = [(s, a) for a in range(256) for s in range(a + 1)]


def probe_channel(pixel: int, channel: int, pair: str) -> tuple[float, float, int, int, str, str]:
    """X1BlendProbes' inputs at pixel.channel: halves nearest k / 255, destination (255 - d, 7d + 3, d, d)."""
    si, ai = CASES[pixel >> 8]
    d = pixel & 255
    dst = (255 - d, (d * 7 + 3) & 255, d, d)[channel]
    s, sa = bm.half(si / 255), bm.half(ai / 255)
    return (sa if channel == 3 else s), sa, dst, d, *PAIRS[pair]


class BlendModelsTest(unittest.TestCase):
    def test_store_rule(self) -> None:
        self.assertEqual(bm.store(0.5), 127)
        self.assertEqual(bm.store(0.25), 64)
        self.assertEqual(bm.store(1.0), 255)
        self.assertEqual(bm.store(float('nan')), 0)
        self.assertEqual([bm.store((k + 0.5) / 255) for k in range(6)], list(range(6)))

    def test_device_first_misses(self) -> None:
        for pair, pixel, channel, device, apple in (
            ('sov', 347, 1, 127, 128),
            ('dov', 624, 0, 144, 143),
            ('sin', 35063, 3, 15, 16),
            ('din', 34824, 0, 15, 16),
        ):
            args = probe_channel(pixel, channel, pair)
            self.assertEqual(bm.chip(*args), device, pair)
            self.assertEqual(bm.apple(*args), apple, pair)

    def test_add_is_exact(self) -> None:
        for pixel in range(0, 8_421_376, 9_973):
            for channel in range(4):
                args = probe_channel(pixel, channel, 'add')
                self.assertEqual(bm.chip(*args), bm.apple(*args))

    def test_one_rounding_of_the_sum(self) -> None:
        args = (bm.half(1 / 255), bm.half(200 / 255), 183, 12, 'dst_alpha', 'one_minus_src_alpha')
        self.assertEqual((bm.chip(*args), bm.per_product(*args)), (40, 39))

    def test_fit_counts_readings(self) -> None:
        reading = (*probe_channel(347, 1, 'sov'), 127)
        self.assertEqual(bm.fit([reading])['chip'], 1)
        self.assertEqual(bm.fit([reading])['apple'], 0)


if __name__ == '__main__':
    unittest.main()
