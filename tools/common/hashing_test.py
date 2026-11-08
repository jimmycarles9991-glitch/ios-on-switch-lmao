import hashlib
import tempfile
import unittest
from pathlib import Path

from tools.common import hashing


class Sha256File(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name)

    def tearDown(self):
        self.dir.cleanup()

    def write(self, name: str, size: int) -> tuple[Path, bytes]:
        data = bytes((i * 131 + size) & 0xFF for i in range(size))
        path = self.root / name
        path.write_bytes(data)
        return path, data

    def test_matches_hashlib_around_the_read_size(self):
        for size in (0, 1, hashing.CHUNK - 1, hashing.CHUNK, hashing.CHUNK + 1, 2 * hashing.CHUNK + 7):
            with self.subTest(size=size):
                path, data = self.write(f'{size}.bin', size)
                self.assertEqual(hashing.sha256_file(path), hashlib.sha256(data).hexdigest())

    def test_advance_counts_every_byte(self):
        path, data = self.write('counted.bin', 2 * hashing.CHUNK + 5)
        seen = []
        hashing.sha256_file(path, seen.append)
        self.assertEqual(sum(seen), len(data))
        self.assertEqual(len(seen), 3)

    def test_many_files_keep_their_order(self):
        written = [self.write(f'{n}.bin', n * 4099) for n in range(9)]
        self.assertEqual(
            hashing.sha256_files([path for path, _ in written]),
            [hashlib.sha256(data).hexdigest() for _, data in written],
        )
        self.assertEqual(hashing.sha256_files([]), [])

    def test_a_missing_file_raises(self):
        with self.assertRaises(FileNotFoundError):
            hashing.sha256_file(self.root / 'absent')


if __name__ == '__main__':
    unittest.main()
