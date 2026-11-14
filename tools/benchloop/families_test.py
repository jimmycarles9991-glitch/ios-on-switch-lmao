import unittest

from tools.benchloop import families, nakbatches, protocol, texels
from tools.benchloop.sm53 import interlock_spin


class FamiliesTest(unittest.TestCase):
    def test_missing_results_still_give_every_line(self):
        planned = [x for _, generate in nakbatches.BATCHES.values() for x in generate()]
        self.assertTrue(families.asked(planned))
        found = families.analyse(planned, {})
        self.assertEqual(found['clock_hz'], families.MEASURED_CLOCK_HZ)
        heads = {line.split()[0] for line in found['summary']}
        for head in ('clock', 'stall', 'stall-wide', 'xmad-mul', 'icache', 'store2fma'):
            self.assertIn(head, heads)
        self.assertTrue(all(v['missing'] for v in found['store2fma'].values()))

    def test_interlock_lines(self):
        found = families.analyse(interlock_spin.generate(), {})
        self.assertIn('interlock.atom.64x64.check missing lost - ns/draw -', found['summary'])
        self.assertIn(
            'interlock.atom.64x64.check-compete missing lost - ns/draw - competitor None buffers', found['summary']
        )

    def test_a_run_of_other_batches_is_not_theirs(self):
        planned = [{'id': 'plain', 'kind': 'noop'}, {'id': 'other', 'kind': 'noop', 'meta': {'family': 'twod'}}]
        self.assertFalse(families.asked(planned))


class BatchFilesTest(unittest.TestCase):
    def test_the_fake_device_stores_every_target_of_every_batch(self):
        seen = set()
        for stem, (_, generate) in nakbatches.BATCHES.items():
            for x in generate():
                fmt, clear = x['target']['format'], tuple(x.get('clear', [0, 0, 0, 0]))
                if (fmt, clear) in seen:
                    continue
                seen.add((fmt, clear))
                stored = texels.encode(clear, fmt)
                self.assertEqual(len(stored), protocol.FORMATS[fmt][1], (stem, x['id']))
                if fmt in texels.UNORM_BITS:
                    want = [texels.unorm(c, n) for c, n in zip(clear, texels.UNORM_BITS[fmt], strict=False)]
                    self.assertEqual(list(texels.codes(stored, fmt)), want, (stem, x['id']))

    def test_files_are_the_generators_output(self):
        for stem in nakbatches.BATCHES:
            path = nakbatches.FOLDER / f'{stem}.json'
            self.assertTrue(path.is_file(), path)
        for stem, text in nakbatches.texts().items():
            self.assertEqual((nakbatches.FOLDER / f'{stem}.json').read_text(), text, stem)


if __name__ == '__main__':
    unittest.main()
