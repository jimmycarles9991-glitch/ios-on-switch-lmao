import os
import unittest
from pathlib import Path

from bootstrap import packages


class ZstdTests(unittest.TestCase):
    def test_known_frame_and_corruption(self):
        library = Path(os.environ['TEST_SRCDIR']) / os.environ['TEST_WORKSPACE'] / 'bootstrap/libzstd.dylib'
        frame = bytes.fromhex('28b52ffd241ad10000626f6f747374726170206465636f64657220666978747572650a8a3ad67a')
        self.assertEqual(packages.decode_zstd(frame, library), b'bootstrap decoder fixture\n')
        with self.assertRaises(ValueError):
            packages.decode_zstd(frame[:-1], library)
        with self.assertRaises(ValueError):
            packages.decode_zstd(b'not a zstd frame', library)


if __name__ == '__main__':
    unittest.main()
