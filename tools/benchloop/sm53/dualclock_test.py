import unittest

from tools.benchloop import protocol
from tools.benchloop.sm53 import dualclock, encode as e, program


class DualClockTest(unittest.TestCase):
    def test_the_slope_is_cycles_a_pair(self):
        planned = dualclock.generate()
        results = {}
        for x in planned[1:]:
            m = x['meta']
            cycles = 500 + (1 if m['delay'] == 0 else 2) * m['pairs']
            texel = program.texel_hex(program.pack(cycles, 12345))
            results[x['id']] = {'status': 'ok', 'texels': {'distinct': [{'value': texel}]}}
        found = dualclock.analyse(planned, results)
        self.assertEqual(found['FFMA+F2F'], {'d0': 1.0, 'd1': 2.0})
        self.assertEqual(found['FFMA+F2F/y'], {'d0': 1.0, 'd1': 2.0})

    def test_the_body_is_timed_between_two_clock_reads(self):
        x = dualclock.experiment('FFMA', 'F2F', 0, True, 16)
        body = e.unbundle(protocol.unb64(x['program']['code']))
        reads = [i for i, ins in enumerate(body) if ins.name.startswith('S2R')]
        self.assertEqual(len(reads), 2)
        between = body[reads[0] : reads[1]]
        pairs = [ins for ins in between if ins.name.split('_')[0] in ('FFMA', 'F2F')]
        self.assertEqual(len(pairs), 32)
        self.assertTrue(all(ins.control.yld for ins in pairs))
        self.assertEqual({ins.control.delay for ins in pairs if ins.name.startswith('FFMA')}, {0})

    def test_one_batch(self):
        self.assertLessEqual(len(dualclock.generate()), protocol.MAX_EXPERIMENTS)


if __name__ == '__main__':
    unittest.main()
