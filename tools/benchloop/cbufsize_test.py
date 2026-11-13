"""The cbufsize batch is valid, probes each bound's edges, and reads a texel as the word it names."""

import struct
import unittest

from tools.benchloop import cbufsize, protocol


def texel(value: float) -> dict:
    return {'status': 'ok', 'texels': {'data': protocol.b64(struct.pack('<4f', *[value] * 4) * 16)}}


class CbufsizeTest(unittest.TestCase):
    def test_the_batch_is_valid_and_names_each_cell_once(self) -> None:
        cells = cbufsize.generate()
        protocol.batch('cbufsize', cells)
        self.assertEqual(len({c['id'] for c in cells}), len(cells))
        self.assertEqual({c['meta']['bound'] for c in cells}, set(cbufsize.SIZES))

    def test_each_bound_is_probed_on_both_sides_of_its_edge(self) -> None:
        self.assertEqual(cbufsize.offsets(64), [0x0, 0x3C, 0x40, 0x100, 0x1DC, 0x29C])
        self.assertIn(0xFC, cbufsize.offsets(255))
        self.assertIn(0x100, cbufsize.offsets(257))

    def test_a_bound_past_the_padded_data_is_refused(self) -> None:
        cell = cbufsize.generate()[0]
        cell['program']['dataBound'] = 1025
        with self.assertRaises(protocol.InvalidError):
            protocol.validate_experiment(cell)

    def test_the_analysis_tells_an_exact_bound_from_a_truncated_one(self) -> None:
        cells = [c for c in cbufsize.generate() if c['meta']['bound'] in (64, 256)]
        exact = {
            c['id']: texel(
                cbufsize.FIRST + c['meta']['offset'] // 4 if c['meta']['offset'] < c['meta']['bound'] else 0.0
            )
            for c in cells
        }
        truncated = {c['id']: texel(0.0) for c in cells}
        self.assertEqual(cbufsize.analyse(cells, exact)[64]['verdict'], 'exact')
        found = cbufsize.analyse(cells, truncated)
        self.assertEqual(found[64]['verdict'], 'differs')
        self.assertEqual(found[64]['zero'], cbufsize.offsets(64))


if __name__ == '__main__':
    unittest.main()
