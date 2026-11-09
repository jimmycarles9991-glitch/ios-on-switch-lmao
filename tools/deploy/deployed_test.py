"""tools/deploy/deployed.py on a throwaway git checkout and a Data volume folder."""

import json
import subprocess
import tempfile
import unittest
from pathlib import Path

from tools.deploy import deployed


class Record(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        (self.root / 'data').mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def git(self, checkout: Path, *args: str) -> str:
        command = ['git', '-C', str(checkout), '-c', 'user.name=t', '-c', 'user.email=t@t', *args]
        return subprocess.run(command, capture_output=True, text=True, check=True).stdout.strip()

    def test_records_the_checkouts_commit_and_whether_it_is_dirty(self):
        checkout = self.root / 'checkout'
        checkout.mkdir()
        self.git(checkout, 'init', '-q', '-b', 'deploy-next5')
        (checkout / 'a.txt').write_text('a')
        self.git(checkout, 'add', 'a.txt')
        self.git(checkout, 'commit', '-q', '-m', 'a')
        head = self.git(checkout, 'rev-parse', 'HEAD')
        lines = []
        info = deployed.record(self.root / 'data', checkout, '24B119', lines.append)
        self.assertEqual(
            (info['commit'], info['branch'], info['dirty'], info['build']), (head, 'deploy-next5', False, '24B119')
        )
        written = json.loads((self.root / 'data/Switch' / deployed.NAME).read_text())
        self.assertEqual(written, info)
        (checkout / 'a.txt').write_text('b')
        self.assertTrue(deployed.record(self.root / 'data', checkout, '', lines.append)['dirty'])

    def test_outside_a_checkout_the_commit_is_empty(self):
        info = deployed.record(self.root / 'data', self.root, '21H461', lambda *_: None)
        self.assertEqual((info['commit'], info['dirty']), ('', False))


if __name__ == '__main__':
    unittest.main()
