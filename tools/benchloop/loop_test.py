"""The whole loop on the fake device, batches through the folder transport and results back.

The fits recover the fake GPU's clock and latencies (fakegpu.py), the calibration batch passes, and a batch whose
process died mid-experiment records it and runs the rest.
"""

import json
import struct
import tempfile
import unittest
from pathlib import Path

from tools.benchloop import doctor, fakedevice, fakegpu, golden, known, loop, protocol, report, session, transport
from tools.benchloop.sm53 import latency, sph


class LatencyLoopTest(unittest.TestCase):
    def test_fits_recover_the_fake_gpu(self) -> None:
        planned = latency.generate(('ladder', 'stall', 'chain'), ops=['FADD', 'HFMA2', 'MUFU.RCP', 'I2F'])
        with tempfile.TemporaryDirectory() as scratch:
            results = session.fake(Path(scratch) / 'device').run_all(planned, 'latency', Path(scratch) / 'out')
            summary = report.write('latency', planned, results, Path(scratch) / 'out', [])
            written = json.loads((Path(scratch) / 'out' / 'report.json').read_text())
        self.assertEqual(len(results), len(planned), summary)
        self.assertAlmostEqual(written['clock']['clock_hz'] / fakegpu.CLOCK_HZ, 1, delta=0.02)
        for op in ('FADD', 'HFMA2'):
            self.assertEqual(written['fixed'][op]['latency'], fakegpu.LATENCIES[op], written['fixed'][op])
        for op, base in (('MUFU.RCP', 'MUFU'), ('I2F', 'I2F')):
            self.assertAlmostEqual(written['variable'][op]['cycles'], fakegpu.LATENCIES[base], delta=1.0)


class CalibrationTest(unittest.TestCase):
    def test_calibration_passes_on_the_fake_device(self) -> None:
        ok, lines = doctor.proven_on_fake()
        self.assertTrue(ok, lines)


class PositionWTest(unittest.TestCase):
    def experiment(self, sph: bytes) -> dict:
        e = next(e for e in golden.golden_batch()['experiments'] if e['id'] == 'fragment')
        return {**e, 'program': {**e['program'], 'sph': protocol.b64(sph)}}

    def test_a_pixel_header_without_position_w_is_killed_and_refused(self) -> None:
        words = list(struct.unpack('<20I', known.KNOWN_SPH))
        words[5] = 0
        e = self.experiment(struct.pack('<20I', *words))
        self.assertEqual(fakegpu.Gpu().run(e).status, 'killed')
        with self.assertRaisesRegex(protocol.InvalidError, 'position W'):
            protocol.validate_experiment(e)

    def test_the_hosts_headers_read_position_w(self) -> None:
        self.assertEqual(fakegpu.Gpu().run(self.experiment(known.KNOWN_SPH)).status, 'ok')
        self.assertEqual(fakegpu.Gpu().run(self.experiment(sph.pixel())).status, 'ok')


class ReuseTest(unittest.TestCase):
    def test_a_batch_name_run_again_runs_again(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch) / 'device'
            device = fakedevice.FakeDevice(root)
            local = transport.Local(root, device.drain)
            first = loop.run(local, golden.golden_batch(), Path(scratch) / 'first')
            device.gpu = fakegpu.Gpu(seed=99)
            second = loop.run(local, golden.golden_batch(), Path(scratch) / 'second')
            self.assertFalse((root / 'outbox' / 'golden').exists())
        self.assertTrue(first.complete)
        self.assertTrue(second.complete)
        self.assertNotEqual(first.results['fragment']['runs'], second.results['fragment']['runs'])

    def test_session_names_are_unique(self) -> None:
        self.assertNotEqual(session.unique('latency-0'), session.unique('latency-0'))
        self.assertTrue(protocol.NAME.match(session.unique('calibration')))


class CrashTest(unittest.TestCase):
    def test_an_experiment_the_process_died_in_is_recorded_and_the_rest_run(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch) / 'device'
            device = fakedevice.FakeDevice(root)
            b = golden.golden_batch()
            (root / 'inbox' / 'golden.json').write_bytes(protocol.encode_batch(b))
            outbox = root / 'outbox' / 'golden'
            outbox.mkdir(parents=True)
            (outbox / 'journal.txt').write_text('starting noop\ndone noop ok\nstarting known\n')
            device.drain()
            journal = (outbox / 'journal.txt').read_text()
            outcome = loop.pull(transport.Local(root), loop.Outcome('golden'), Path(scratch) / 'out')
        self.assertIn('done known crashed', journal)
        self.assertNotIn('starting noop\nstarting noop', journal)
        self.assertEqual(outcome.results['known']['status'], 'crashed')
        self.assertEqual(outcome.results['fragment']['status'], 'ok')
        self.assertTrue(outcome.complete)


if __name__ == '__main__':
    unittest.main()
