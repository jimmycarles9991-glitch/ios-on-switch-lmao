#!/usr/bin/env python3
"""The lines naming a state the pack lacks are opt-in stats and short: no key in a log line, which a long one cuts.

The miss line (TegraGM20BMetalStatesMiss.mm gm20bNoteMiss) is under SWITCH_METAL_STATS and names the state by its
hash and label; its key goes to the harvest parts under that hash, which logharvest joins. The compute state errors
(TegraGM20BMetalComputeState.mm), which stay in every build, name the hash too, once a process each.
usage: miss_lines_source_test.py <TegraGM20BMetalStatesMiss.mm> <TegraGM20BMetalComputeState.mm>
"""

import re
import sys
import unittest
from pathlib import Path

LOG_CALL = re.compile(r'os_log(?:_error)?\((?:[^;]|\n)*?\);')


class MissLines(unittest.TestCase):
    def test_the_miss_line_is_debug_only_and_keyless(self):
        miss = Path(sys.argv[1]).read_text()
        body = miss[miss.index('void gm20bNoteMiss(') :]
        debug = body[body.index('#if SWITCH_METAL_STATS') : body.index('#else')]
        calls = LOG_CALL.findall(debug)
        self.assertEqual(len(calls), 1)
        self.assertNotIn('key', calls[0].replace('keyHash', ''))
        self.assertIn('logHarvestParts(keyHash, label, key)', debug)
        self.assertEqual(LOG_CALL.findall(body[body.index('#else') :]), [])

    def test_the_compute_errors_carry_no_key_and_are_named_once(self):
        compute = Path(sys.argv[2]).read_text()
        for call in (c for c in LOG_CALL.findall(compute) if 'no compute state %{public}s (%{public}s)' in c):
            self.assertNotIn('key.c_str()', call)
        self.assertEqual(len(re.findall(r'if \(gm20bFirstNaming\(keyHash, [12]\)\)', compute)), 2)


if __name__ == '__main__':
    unittest.main(argv=sys.argv[:1])
