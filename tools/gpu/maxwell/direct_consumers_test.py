"""The native direct compiler's linked-fragment and neutral-texture consumers."""

import subprocess
import sys
import unittest
from pathlib import Path

RUNNER = Path()
PACK = Path()


class DirectConsumers(unittest.TestCase):
    def test_linked_fragment_is_the_inline_program_and_missing_callee_is_refused(self):
        result = subprocess.run(
            [
                RUNNER / 'linkcheck',
                PACK / 'pack.bin',
                RUNNER / 'linked-caller.metallib',
                RUNNER / 'linked-callee.metallib',
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('byte for byte', result.stdout)
        self.assertIn('unlinked refused:', result.stdout)

    def test_neutral_and_bound_texture_entries_keep_their_sampler(self):
        result = subprocess.run(
            [RUNNER / 'neutral_compute', RUNNER / 'neutral.metallib'],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        expected = [
            f'PASS {name} texture={bound} sampler=unbound'
            for name in ('neutral_read', 'neutral_sample', 'neutral_store', 'neutral_compare')
            for bound in ('unbound', 'bound')
        ]
        self.assertEqual([line for line in result.stdout.splitlines() if line.startswith('PASS ')], expected)


if __name__ == '__main__':
    RUNNER, PACK = map(Path, sys.argv[1:3])
    unittest.main(argv=sys.argv[:1])
