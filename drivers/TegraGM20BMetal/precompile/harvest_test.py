"""harvest.py finds a system process's m2v cache by its container's identifier, whatever UUID the install gave it."""

import plistlib
import tempfile
import unittest
from pathlib import Path

from drivers.TegraGM20BMetal.precompile import harvest


def container(volume: Path, uuid: str, identifier: str) -> Path:
    root = volume / 'containers/Data/System' / uuid
    (root / 'Library/Caches/m2v').mkdir(parents=True)
    with (root / harvest.CONTAINER_METADATA).open('wb') as f:
        plistlib.dump({'MCMMetadataIdentifier': identifier}, f)
    (root / 'Library').chmod(0)
    return root


class Caches(unittest.TestCase):
    def test_a_closed_system_container_is_opened_by_its_identifier(self):
        with tempfile.TemporaryDirectory() as d:
            volume = Path(d)
            backboardd = container(volume, 'CE929FE0-E622-45FA-8423-A8020D7412A1', 'com.apple.backboardd')
            other = container(volume, '00000000-0000-0000-0000-000000000000', 'com.apple.locationd')
            try:
                found, skipped = harvest.caches(volume, set(harvest.SYSTEM_CACHES), False, print)
                self.assertEqual([m2v for _, m2v, _ in found], [backboardd / 'Library/Caches/m2v'])
                self.assertEqual(skipped, [other.name])
                found, skipped = harvest.caches(volume, {other.name}, False, print, explicit=True)
                self.assertEqual([m2v for _, m2v, _ in found], [other / 'Library/Caches/m2v'])
                self.assertEqual(skipped, [backboardd.name])
            finally:
                for root in (backboardd, other):
                    (root / 'Library').chmod(0o755)


if __name__ == '__main__':
    unittest.main()
