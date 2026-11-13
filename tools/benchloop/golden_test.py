"""protocol.py still writes testdata/golden_batch.json byte for byte (the app's host test decodes the same file)."""

import os
import unittest
from pathlib import Path

from tools.benchloop import golden, protocol

GOLDEN = Path(__file__).with_name('testdata') / 'golden_batch.json'


class GoldenTest(unittest.TestCase):
    def test_writer_reproduces_the_golden_batch(self) -> None:
        written = protocol.encode_batch(golden.golden_batch())
        if os.environ.get('BENCHLOOP_REGOLDEN'):
            GOLDEN.write_bytes(written)
        self.assertEqual(written, GOLDEN.read_bytes())


if __name__ == '__main__':
    unittest.main()
