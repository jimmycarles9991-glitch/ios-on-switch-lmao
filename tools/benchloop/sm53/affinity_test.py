import struct
import unittest

from tools.benchloop import protocol
from tools.benchloop.sm53 import affinity, encode, program


def summary(pairs: dict[tuple[int, int], int]) -> dict:
    return {'distinct': [{'value': program.texel_hex(program.pack(*k)), 'count': n} for k, n in pairs.items()]}


def summary_r32(values: dict[int, int]) -> dict:
    words = {struct.pack('<I', program.ONE | v).hex(): n for v, n in values.items()}
    return {'distinct': [{'value': w, 'count': n} for w, n in words.items()]}


class AffinityTest(unittest.TestCase):
    def test_one_valid_batch_virtid_last(self):
        experiments = affinity.generate()
        protocol.batch('sr-affinity', experiments)
        self.assertEqual(experiments[-1]['id'], 'affinity.virtid')

    def test_code_reads_the_affinity_register(self):
        words = encode.unbundle(affinity.code(affinity.SR_VIRTID))
        s2r = [w.word for w in words if w.name == 'S2R']
        self.assertEqual([w >> 20 & 0xFF for w in s2r], [0x1C, 0x03])
        self.assertEqual(encode.barrier_hazards(words), [])

    def test_histogram_from_summary_and_full(self):
        experiments = affinity.generate()
        pixels = 1280 * 720
        tile = 256 * 128
        full = b''.join(struct.pack('<4I', *program.pack(0x0100 * (i % 2), 0)) for i in range(tile))
        results = {
            'affinity.screen': {'status': 'ok', 'texels': summary_r32({0x00: pixels - 64, 0x0102: 64})},
            'affinity.tile': {'status': 'ok', 'texels': {'data': protocol.b64(full)}},
            'affinity.screen4': {'status': 'killed', 'why': 'trap'},
        }
        found = affinity.analyse(experiments, results)
        self.assertEqual(found['affinity.screen']['histogram'], {0: pixels - 64, 2: 64})
        self.assertEqual((found['affinity.screen']['max'], found['affinity.screen']['fits']), (2, True))
        self.assertEqual(found['affinity.tile']['histogram'], {0: tile})
        self.assertEqual(found['affinity.tile']['whole_values'], ['0x00000000', '0x00000100'])
        self.assertEqual(found['affinity.screen4']['status'], 'killed')
        self.assertEqual(found['affinity.virtid']['status'], 'missing')

    def test_buffer_needed_past_four_gpcs(self):
        experiments = affinity.generate()
        results = {'affinity.tile': {'status': 'ok', 'texels': summary({(5, 5): 10})}}
        found = affinity.analyse(experiments, results)['affinity.tile']
        self.assertEqual((found['max'], found['ticket_buffer_kib'], found['fits']), (5, 6, False))


if __name__ == '__main__':
    unittest.main()
