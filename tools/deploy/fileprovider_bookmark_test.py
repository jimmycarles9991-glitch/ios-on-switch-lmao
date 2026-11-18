import hashlib
import sqlite3
import tempfile
import unittest
from pathlib import Path

from tools.deploy import fileprovider_bookmark as bookmarks

CONSUMER = 'com.brunostjohn.switch.installer'
KEY = bytes.fromhex('b2522a17a3c85c8fb165e62756d97b0edbdc3162212ea130d9001b214d9aa2eb')
SESSION = b'installed-session'
BOOKMARK = 'fp:/itBXMIriRAaoE5iX+b0_2pXmFJ2f23JyjfBUqFAWe4M=/com.apple.FileProvider.LocalStorage//did=3829'


class FileProviderBookmarkTest(unittest.TestCase):
    def test_apple_generated_bookmark(self):
        self.assertTrue(bookmarks.hmac_matches(BOOKMARK, KEY, CONSUMER, SESSION))

    def test_caller_must_match_the_issued_consumer_and_session(self):
        for consumer, session in [
            (CONSUMER, None),
            (CONSUMER, b'different-session'),
            ('com.brunostjohn.switch.other', SESSION),
            ('SWITCHTEAM.' + CONSUMER, SESSION),
        ]:
            with self.subTest(consumer=consumer, session=session):
                self.assertFalse(bookmarks.hmac_matches(BOOKMARK, KEY, consumer, session))

    def test_wrong_key_or_changed_item_is_rejected(self):
        self.assertFalse(bookmarks.hmac_matches(BOOKMARK, bytes(32), CONSUMER, SESSION))
        self.assertFalse(bookmarks.hmac_matches(BOOKMARK.replace('3829', '3830'), KEY, CONSUMER, SESSION))

    def test_malformed_bookmarks_are_rejected(self):
        for bookmark in ['', 'file:///package.ipa', 'fp:/!/provider//item', 'fp:/YQ==/provider//item', 'fp:/YQ==/']:
            with self.subTest(bookmark=bookmark), self.assertRaises(ValueError):
                bookmarks.parse(bookmark)

    def test_database_is_read_only_and_does_not_expose_keys(self):
        with tempfile.TemporaryDirectory() as temporary:
            database = Path(temporary) / 'AccessControl.db'
            connection = sqlite3.connect(database)
            connection.execute('CREATE TABLE bundle_keys (identifier TEXT, symmetric_key BLOB)')
            connection.executemany(
                'INSERT INTO bundle_keys VALUES (?, ?)', [(CONSUMER, KEY), ('com.brunostjohn.switch.other', bytes(32))]
            )
            connection.commit()
            connection.close()
            before = hashlib.sha256(database.read_bytes()).digest()
            result = bookmarks.inspect(database, BOOKMARK, {'legacy': None, 'registered': SESSION})
            self.assertEqual(
                result,
                [
                    {'consumer': CONSUMER, 'matching_hmac_sessions': ['registered']},
                    {'consumer': 'com.brunostjohn.switch.other', 'matching_hmac_sessions': []},
                ],
            )
            self.assertEqual(hashlib.sha256(database.read_bytes()).digest(), before)
            self.assertNotIn(KEY.hex(), str(result))

    def test_missing_database_is_not_created(self):
        with tempfile.TemporaryDirectory() as temporary:
            database = Path(temporary) / 'missing.db'
            with self.assertRaises(FileNotFoundError):
                bookmarks.inspect(database, BOOKMARK, {'legacy': None})
            self.assertFalse(database.exists())

    def test_uncheckpointed_keys_are_read_without_changing_wal_or_shared_memory(self):
        with tempfile.TemporaryDirectory() as temporary:
            database = Path(temporary) / 'AccessControl.db'
            connection = sqlite3.connect(database)
            try:
                connection.execute('PRAGMA journal_mode=WAL')
                connection.execute('CREATE TABLE bundle_keys (identifier TEXT, symmetric_key BLOB)')
                connection.execute('INSERT INTO bundle_keys VALUES (?, ?)', (CONSUMER, KEY))
                connection.commit()
                before = {path.name: path.read_bytes() for path in database.parent.iterdir()}
                result = bookmarks.inspect(database, BOOKMARK, {'registered': SESSION})
                self.assertEqual(result, [{'consumer': CONSUMER, 'matching_hmac_sessions': ['registered']}])
                self.assertEqual({path.name: path.read_bytes() for path in database.parent.iterdir()}, before)
            finally:
                connection.close()


if __name__ == '__main__':
    unittest.main()
