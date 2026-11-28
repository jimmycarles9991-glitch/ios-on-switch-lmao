#!/usr/bin/env python3
"""The pack's launch set holds every Core Animation state a device or QuartzCore names (make_pack.py COLD_TIERS).

A state of tier 1 runs only after its programs are imported in the process that asks for it, so whatever QuartzCore
asks for at startup has to be tier 0's: every manifest state in each form the device asks it in (deepcolour.asked:
ca_asked.py's states, HW Test gpu.pack.hit_cost's), every state a device's logs named (ca-names) and every one the
devices made (demand-24B119.json). Only the generator's own expansions carry the tier-1 mark.
"""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import caset  # noqa: E402
import caspec  # noqa: E402
import closure  # noqa: E402
import deepcolour  # noqa: E402
import make_pack  # noqa: E402

CORPUS = os.path.join(HERE, '..', '..', 'precompile', 'corpus.log')
CA_NAMES = os.path.join(HERE, '..', '..', 'precompile', 'ca-names', '24B119.txt')


class _LaunchSetBase(unittest.TestCase):
    BUILD: str
    QC_FILE: str
    CA_NAMES_FILE: str | None = None

    @classmethod
    def setUpClass(cls) -> None:
        if cls is _LaunchSetBase:
            raise unittest.SkipTest('base class')
        qc = os.path.join(HERE, cls.QC_FILE)
        cls.tables = caspec.Tables(qc)
        cls.names = []
        if cls.CA_NAMES_FILE:
            with open(cls.CA_NAMES_FILE) as f:
                cls.names = f.read().split()
        cls.tiers = make_pack.ca_states(make_pack.Path(qc), make_pack.Path(CORPUS), cls.names)
        cls.launch = {
            make_pack.state_key(line) for tier, lines in cls.tiers if tier not in make_pack.COLD_TIERS for line in lines
        }

    def test_every_state_quartzcore_asks_for_is_in_the_launch_set(self):
        asked = [
            form
            for m in self.tables.t['manifest']
            if m[0][0] == 'P'
            for form in [m[0], *deepcolour.asked(self.tables, m[0], None)]
        ]
        named = [n for n in self.names if caset.makes(self.tables, n)] + closure.demand(self.BUILD)
        self.assertGreater(len(asked), 800)
        missing = [
            n for n in asked + named if make_pack.state_key(caspec.state_line(self.tables, n)) not in self.launch
        ]
        self.assertEqual(missing, [])

    def test_only_the_generators_expansions_are_marked_tier_one(self):
        for tier, lines in self.tiers:
            marked = {' tier=1 ' in line for line in lines}
            self.assertEqual(marked, {tier in make_pack.COLD_TIERS} if lines else set(), tier)
        self.assertGreater(sum(len(lines) for tier, lines in self.tiers if tier in make_pack.COLD_TIERS), 50_000)


class LaunchSet24B119(_LaunchSetBase):
    BUILD = '24B119'
    QC_FILE = 'qc-24B119.json'
    CA_NAMES_FILE = CA_NAMES


class LaunchSet21H461(_LaunchSetBase):
    BUILD = '21H461'
    QC_FILE = 'qc-21H461.json'


if __name__ == '__main__':
    unittest.main()
