import unittest

from tools.benchloop import protocol
from tools.benchloop.sm53 import encode, icache


class IcacheTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.experiments = icache.generate()

    def test_one_valid_batch_spanning_the_footprints(self):
        protocol.batch('icache', self.experiments)
        sizes = [x['meta']['code_bytes'] for x in self.experiments[1:]]
        self.assertLess(sizes[0], 2 * 1024)
        self.assertGreater(sizes[-1], 60 * 1024)
        self.assertLessEqual(sizes[-1], protocol.MAX_CODE)

    def test_every_footprint_runs_the_same_ffmas(self):
        texels = {tuple(x['expect']['texel']) for x in self.experiments[1:]}
        self.assertEqual(len(texels), 1)
        accumulator = icache.SEGMENTS_RUN * -(-icache.FFMAS // 16) * 0.5
        self.assertEqual(icache.program.unpack(list(texels.pop()))[0], encode.f32(accumulator))

    def test_segments_are_whole_kib(self):
        self.assertEqual(icache.SEGMENT % 3, 0)
        self.assertEqual(icache.SEGMENT // 3 * 32, 1024)
        one, two = (icache.code(n)[0] for n in (1, 2))
        self.assertEqual(len(two) - len(one), 1024)

    def test_the_large_footprint_needs_more_than_64_kib(self):
        (x,) = icache.generate_large()[1:]
        self.assertGreater(x['meta']['code_bytes'], 64 * 1024)
        protocol.validate_experiment(x)

    def test_analysis(self):
        results = {
            x['id']: {
                'status': 'ok',
                'runs': [{'draws': d, 'gpu_ns': [d * 2e6 * x['meta'].get('segments', 1)]} for d in icache.DRAWS],
                'texels': {'distinct': []},
            }
            for x in self.experiments
        }
        rows = icache.analyse(self.experiments, results, 1e9)
        self.assertEqual(len(rows), len(icache.FOOTPRINTS))
        self.assertAlmostEqual(rows[0]['cycles_per_pixel'], 2e6 / (256 * 128))


if __name__ == '__main__':
    unittest.main()
