import hashlib
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools.gpu.probe import library_view


def content_name(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest() + '.metallib'


class NativeLibraries(unittest.TestCase):
    def test_crawled_and_harvested_libraries_share_content_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sources = []
            for name, content in [('embedded', b'embedded AIR'), ('device', b'CI AIR'), ('duplicate', b'CI AIR')]:
                source = root / name
                source.write_bytes(content)
                sources.append(source)
            view = library_view(sources, [], root / 'view')
            self.assertEqual(len(list(view.iterdir())), 2)
            for source in sources:
                self.assertEqual((view / content_name(source.read_bytes())).read_bytes(), source.read_bytes())

    def test_a_crawled_library_is_linked_by_its_name_unread(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            crawled = root / ('ab' * 32 + '.metallib')
            crawled.write_bytes(b'bytes that do not hash to the name')
            with mock.patch.object(Path, 'read_bytes', side_effect=AssertionError('a crawled library was read')):
                view = library_view([], [crawled], root / 'view')
            self.assertEqual([p.name for p in view.iterdir()], [crawled.name])
            self.assertEqual((view / crawled.name).resolve(), crawled.resolve())

    def test_a_device_library_wins_over_the_crawled_one_of_its_hash(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            device = root / 'device'
            device.write_bytes(b'CI AIR')
            crawled = root / content_name(b'CI AIR')
            crawled.write_bytes(b'CI AIR')
            view = library_view([device], [crawled], root / 'view')
            self.assertEqual([p.name for p in view.iterdir()], [crawled.name])
            self.assertEqual((view / crawled.name).resolve(), device.resolve())

    def test_a_crawled_library_not_named_by_its_hash_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            stray = root / 'stray.metallib'
            stray.write_bytes(b'AIR')
            with self.assertRaises(ValueError):
                library_view([], [stray], root / 'view')

    def test_missing_declared_input_fails_instead_of_silently_losing_coverage(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaises(FileNotFoundError):
                library_view([root / 'missing'], [], root / 'view')


if __name__ == '__main__':
    unittest.main()
