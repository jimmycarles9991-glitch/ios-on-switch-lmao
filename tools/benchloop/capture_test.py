"""A split capture as the device logs it replays through the pack kind byte for byte.

testdata/split_capture.log is what raw_split_capture_test.cpp checks the device's lines against; the batch made from it
is testdata/split_capture_batch.json, which the app's host test decodes.
"""

import unittest
from pathlib import Path

from tools.benchloop import capture, protocol

TESTDATA = Path(__file__).with_name('testdata')
LOG = (TESTDATA / 'split_capture.log').read_text()
FRAGMENT = bytes(range(64))
VERTICES = bytes(255 - i for i in range(64))


class CaptureTest(unittest.TestCase):
    def test_the_record_is_the_one_the_device_wrote(self) -> None:
        blobs = capture.blobs(LOG)
        self.assertEqual(list(blobs), [7])
        r = capture.record(blobs[7])
        self.assertEqual(r['key'], 0x618B45175101CD20)
        self.assertEqual((r['format'], r['width'], r['height'], r['box']), (115, 832, 1408, [20, 954, 129, 1280]))
        self.assertEqual((r['primitive'], r['first'], r['count'], r['ca']), (4, 0, 4, True))
        self.assertEqual(r['push'], [i * 3 for i in range(32)])
        self.assertEqual(r['vertices'], VERTICES)
        self.assertEqual(
            r['blocks'],
            [
                {'stage': 2, 'slot': 1, 'offset': 80, 'data': FRAGMENT},
                {'stage': 1, 'slot': 0, 'offset': 0, 'data': bytes([0x3C] * 64)},
            ],
        )

    def test_each_form_replays_the_captured_bytes(self) -> None:
        b = capture.batch(LOG, 'split-capture-batch')
        self.assertEqual([e['pack']['form'] for e in b['experiments']], list(protocol.PACK_FORMS))
        for e in b['experiments']:
            quad = e['pack']['quads'][0]
            self.assertEqual(e['template'], '618b45175101cd20')
            self.assertEqual(protocol.unb64(quad['vertices']), VERTICES)
            self.assertEqual(protocol.unb64(quad['ubos'][0]['data']), FRAGMENT)
            self.assertEqual(quad['ubos'][0]['offset'], 80)
            self.assertEqual(quad['rect'], [20, 954, 129, 1280])

    def test_a_record_missing_a_part_is_left_out(self) -> None:
        lines = [line for line in LOG.splitlines() if 'splitcap 7.1 ' not in line]
        self.assertEqual(capture.blobs('\n'.join(lines)), {})

    def test_the_batch_is_the_one_the_app_decodes(self) -> None:
        written = protocol.encode_batch(capture.batch(LOG, 'split-capture-batch'))
        self.assertEqual(written, (TESTDATA / 'split_capture_batch.json').read_bytes())


if __name__ == '__main__':
    unittest.main()
