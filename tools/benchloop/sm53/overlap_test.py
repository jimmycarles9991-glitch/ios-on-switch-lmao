import unittest

from tools.benchloop import protocol
from tools.benchloop.sm53 import operands, overlap, program

RATES = {'FFMA': 1.0, 'F2F': 4.0, 'SHL': 2.0, 'MUFU.RCP': 4.0, 'ISETP': 2.25}


class OverlapTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.experiments = overlap.generate()

    def test_one_valid_batch_with_one_register_count(self):
        protocol.batch('overlap', self.experiments)
        streams = self.experiments[1:]
        self.assertEqual(len(streams), len(overlap.STREAMS) * len(overlap.LOOPS))
        self.assertEqual({x['program']['gprs'] for x in streams}, {overlap.GPRS})
        for x in streams:
            highest = operands.highest_register(protocol.unb64(x['program']['code']))
            self.assertLess(highest, overlap.GPRS)

    def test_mixes_keep_their_ratio(self):
        for name, group in overlap.MIXES.items():
            names = [i.name.split('_')[0] for i in overlap.body(group)]
            want = {'FFMA': 'FFMA', 'F2F': 'F2F', 'SHL': 'SHL', 'MUFU.RCP': 'MUFU', 'ISETP': 'ISETP'}
            self.assertEqual(names[: len(group)], [want[c] for c in group], name)
            self.assertEqual(len(names), overlap.BODY)

    def test_predictions(self):
        self.assertEqual(overlap.predictions(RATES, ('FFMA', 'FFMA', 'FFMA', 'F2F')), {'overlap': 1.0, 'add': 1.75})
        self.assertEqual(overlap.predictions(RATES, ('FFMA', 'SHL')), {'overlap': 1.0, 'add': 1.5})

    def test_analysis_picks_the_closer_model(self):
        clock = 1e9
        warps = 256 * 128 // 32
        rates = RATES | {'FFMA3-F2F1': 1.02, 'FFMA1-SHL1': 1.5, 'FFMA3-MUFU1': 1.75, 'FFMA1-ISETP1': 1.1}
        results = {}
        for x in self.experiments[1:]:
            ns = rates[x['meta']['op']] * x['meta']['k'] * warps / 8
            texel = program.texel_hex(x['expect']['texel'])
            results[x['id']] = {
                'status': 'ok',
                'runs': [{'draws': d, 'gpu_ns': [5000 + d * ns]} for d in (1, 2, 4, 8)],
                'texels': {'distinct': [{'value': texel, 'count': 256 * 128}]},
            }
        found = overlap.analyse(self.experiments, results, clock)
        self.assertEqual(found['FFMA3-F2F1']['closer'], 'overlap')
        self.assertEqual(found['FFMA1-SHL1']['closer'], 'add')
        self.assertAlmostEqual(found['cycles']['FFMA']['cycles'], 1.0)


if __name__ == '__main__':
    unittest.main()
