import hashlib
import tempfile
import unittest
from pathlib import Path

from tools.gpu.rawprobe import library_view


class NativeLibraries(unittest.TestCase):
    def test_crawled_and_harvested_libraries_share_content_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sources = []
            for name, content in [('embedded', b'embedded AIR'), ('device', b'CI AIR'), ('duplicate', b'CI AIR')]:
                source = root / name
                source.write_bytes(content)
                sources.append(source)
            view = library_view(sources, root / 'view')
            self.assertEqual(len(list(view.iterdir())), 2)
            for source in sources:
                digest = hashlib.sha256(source.read_bytes()).hexdigest()
                self.assertEqual((view / (digest + '.metallib')).read_bytes(), source.read_bytes())

    def test_missing_declared_input_fails_instead_of_silently_losing_coverage(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaises(FileNotFoundError):
                library_view([root / 'missing'], root / 'view')


if __name__ == '__main__':
    unittest.main()
