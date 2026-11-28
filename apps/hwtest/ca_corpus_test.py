"""gpu/CACorpus.swift's states against where they come from: corpus.log's lines, and caspec.py with 24B119's tables."""

import importlib.util
import os
import re
import sys
import unittest
from pathlib import Path

# caspec imports its iOS 27 half (caspec27) by its bare name, so ca/ goes on the path and caspec is imported as caspec
sys.path.insert(0, str(Path(importlib.util.find_spec('drivers.TegraGM20BMetal.pack.ca.caspec').origin).parent))
import caspec


def block(swift: str, name: str) -> list[str]:
    match = re.search(rf'static let {name} = """\n(.*?)\n\s*"""', swift, re.DOTALL)
    assert match, name
    return [line.strip() for line in match.group(1).split('\n') if line.strip()]


def words(line: str) -> list[str]:
    """Give a line's words without the corpus's lib= and os= tags, which CACorpus leaves out."""
    return [w for w in line.split() if not w.startswith(('lib=', 'os='))]


def label(line: str) -> str:
    return line.split()[2].rpartition('.')[2]


class Corpus(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.swift = Path(os.environ['CA_CORPUS_SWIFT']).read_text()
        cls.corpus = Path(os.environ['CA_CORPUS']).read_text().splitlines()
        cls.tables = caspec.Tables(os.environ['QC_24B119'])

    def test_every_line_is_the_corpuss(self) -> None:
        lines = block(self.swift, 'lines')
        self.assertIn('Pb3a8BsovXmw_A3Xhfx', [label(line) for line in lines])
        for line in lines:
            drawn = [words(c) for c in self.corpus if c.startswith('R ') and label(c) == label(line)]
            self.assertIn(words(line), drawn, label(line))

    def test_every_ios_27_line_is_the_one_caspec_makes(self) -> None:
        lines = block(self.swift, 'lines27')
        self.assertIn('Pb3a8BsovXmw_A3Xhfx', [label(line) for line in lines])
        for line in lines:
            self.assertEqual(line, caspec.state_line(self.tables, label(line)).strip(), label(line))


if __name__ == '__main__':
    unittest.main()
