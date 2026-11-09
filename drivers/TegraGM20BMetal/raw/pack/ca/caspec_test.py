#!/usr/bin/env python3
"""caspec.py makes the corpus's Core Animation states from their names.

A harvested line is authoritative: the pack keeps it as the device drew it (rawpack.py ca_corpus), and a name supplies
the states no journal holds. A device draws some names otherwise than QuartzCore 17's tables make them (iOS 27's glass,
an attachment of another format), so the names must make nearly every harvested line, not each one.
"""

import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import caspec  # noqa: E402

CORPUS = os.path.join(HERE, '..', '..', '..', 'precompile', 'corpus.log')


class CorpusStates(unittest.TestCase):
    def test_names_make_the_harvested_states(self):
        tables = caspec.Tables(os.path.join(HERE, 'qc-21H461.json'))
        made = other = 0
        with open(CORPUS) as f:
            for line in f:
                name, want = caspec.corpus_fields(line)
                if not name:
                    continue
                try:
                    fields = caspec.state_fields(tables, name)
                except (KeyError, IndexError, ValueError):
                    fields = None
                if fields != want:
                    other += 1
                    continue
                # the functions' part as the probe keys it, blanks and all (raw_metal_pipeline_key's spacing)
                got = caspec.state_line(tables, name)
                journal = re.sub(r' lib=\S+', '', line.rstrip('\n'))
                self.assertEqual(got[got.index(' | v ') :], journal[journal.index(' | v ') :], name)
                made += 1
        self.assertGreater(made, 600)
        self.assertLess(other, made // 20)


if __name__ == '__main__':
    unittest.main()
