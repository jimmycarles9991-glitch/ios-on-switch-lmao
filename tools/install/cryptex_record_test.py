"""cryptex_record: a staged image's sha256 stands only for the file it was written to; the install writes it."""

import hashlib
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools.install import cryptex_record, populate

SHA = 'ab' * 32


class Log:
    def __call__(self, *parts: object) -> None:
        pass

    quiet = __call__


class CryptexRecordTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.image = self.root / 'image.dmg'
        self.image.write_bytes(b'image')

    def tearDown(self):
        self.tmp.cleanup()

    def test_a_record_names_the_file_it_was_written_for(self):
        self.assertIsNone(cryptex_record.read(self.image), 'no record')
        cryptex_record.write(self.image, SHA)
        self.assertEqual(cryptex_record.read(self.image), SHA)
        self.assertEqual(json.loads(cryptex_record.path(self.image).read_text())['sha256'], SHA)

    def test_a_replaced_or_changed_file_has_no_record(self):
        cryptex_record.write(self.image, SHA)
        other = self.root / 'other.dmg'
        other.write_bytes(b'image')
        other.rename(self.image)
        self.assertIsNone(cryptex_record.read(self.image), 'a new file under the name')
        cryptex_record.write(self.image, SHA)
        os.utime(self.image, ns=(1, 1))
        self.assertIsNone(cryptex_record.read(self.image), 'modified since')
        cryptex_record.write(self.image, SHA)
        self.image.write_bytes(b'image, longer')
        self.assertIsNone(cryptex_record.read(self.image), 'another size')

    def test_an_older_or_unreadable_record_is_none(self):
        for text in (SHA + '\n', '{', '[]', '{"sha256": 1}'):
            with self.subTest(text=text):
                cryptex_record.path(self.image).write_text(text)
                self.assertIsNone(cryptex_record.read(self.image))


class PopulateRecordsTest(unittest.TestCase):
    """The install writes the record beside each image it stages, so a deploy never reads one back."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.preboot = self.root / 'preboot'
        self.preboot.mkdir()
        self.source = self.root / 'source' / '141-10410-227.dmg'
        self.source.parent.mkdir()
        self.source.write_bytes(b's' * 4096)
        self.want = hashlib.sha256(self.source.read_bytes()).hexdigest()
        self.spec = {
            'cryptexes': [{'member': 'system_cryptex', 'path': '/private/preboot/Switch-Staging/141-10410-227.dmg'}]
        }

    def tearDown(self):
        self.tmp.cleanup()

    def stage(self) -> None:
        with mock.patch.object(populate.host, 'run'):
            populate.stage_cryptexes(self.preboot, self.spec, {'system_cryptex': (self.source, self.want)}, None, Log())

    def test_a_staged_image_is_recorded_and_found_again_without_hashing_it(self):
        self.stage()
        target = self.preboot / 'Switch-Staging/141-10410-227.dmg'
        self.assertEqual(cryptex_record.read(target), self.want)
        with mock.patch.object(populate.hashing, 'sha256_file', side_effect=AssertionError('read back')):
            self.stage()
        self.assertEqual(target.read_bytes(), self.source.read_bytes())


if __name__ == '__main__':
    unittest.main()
