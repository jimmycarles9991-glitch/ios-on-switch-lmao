"""The option and iterated-blend sweeps: valid on the wire, every option put back, one target, the fake device."""

import tempfile
import unittest
from pathlib import Path

from tools.benchloop import protocol, report, session, sweeps

TABLE = """name\ttags\tmethod\tvalue\tdecoded
probe.mov\tff_alpha\t0x0df0\t0x00000000\t
probe.mov\tff_alpha\t0x0df4\t0x00005001\tInsn(...)
probe.mov\tff_alpha\t0x0dd4\t0x00000001\t
probe.mov\tff_alpha\t0x0dd0\t0x00000001\t
"""


class SweepsTest(unittest.TestCase):
    def test_precision_experiments_are_valid_and_put_every_option_back(self) -> None:
        experiments = sweeps.precision()
        self.assertLessEqual(len(experiments), protocol.MAX_EXPERIMENTS)
        for e in experiments:
            protocol.validate_experiment(e)
            touched = {m for m, _ in e.get('methods', [])} - {0x1E00 + 4 * k for k in range(7)} - {0x12E4}
            self.assertLessEqual(touched, {m for m, _ in e.get('restore', [])}, e['id'])

    def test_iterated_experiments_bind_colour_zero_alone(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            table = Path(scratch) / 'microcode.tsv'
            table.write_text(TABLE)
            experiments = sweeps.iterated(table)
        self.assertEqual(experiments[-1]['methods'], [[0x0DF0, 0], [0x0DF4, 0x5001], [0x0DD4, 1], [0x0DD0, 1]])
        for e in experiments:
            protocol.validate_experiment(e)
            self.assertEqual(e['targets'], 1)
            self.assertFalse(protocol.unb64(e['program']['sph'])[1] & 1 << 6, e['id'])
            self.assertIn([0x0DD0, 0], e['methods' if e['meta'].get('primer') else 'restore'])
        primers, drawn = experiments[0::2], experiments[1::2]
        self.assertEqual([p['id'] for p in primers], ['pr-' + e['id'] for e in drawn])
        self.assertTrue(all(p['fill'] == e['fill'] for p, e in zip(primers, drawn, strict=True)))

    def test_precision_runs_on_the_fake_device(self) -> None:
        planned = [e for e in sweeps.precision() if e['meta']['option'] in ('base', 'color-clamp-off')][:4]
        with tempfile.TemporaryDirectory() as scratch:
            results = session.fake(Path(scratch) / 'device').run_all(planned, 'precision', Path(scratch) / 'out')
        rows = report.analysis('precision', planned, results, [])['precision']
        self.assertEqual([r['status'] for r in rows], ['ok'] * len(planned))
        self.assertTrue(all(r.get('same_as_base', True) for r in rows))


if __name__ == '__main__':
    unittest.main()
