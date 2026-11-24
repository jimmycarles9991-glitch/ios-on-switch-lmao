#!/usr/bin/env python3
"""closure.py and caname.py: the pack holds every context of every known Core Animation content, by QuartzCore's names.

The name codec is held to the names QuartzCore itself wrote: its offline manifest and the states devices made
(demand-24B119.json). The table's rules are held to the states 24B119's QuartzCore named (the manifest, the demand
table, the corpus's 24B119 lines): each lies inside its own content's closure. The closure is held to the pack's own
corpus lines (make_pack.ca_states): each seed's content under each context value the table gives has a line, and a
missing one names its dimension. What the table leaves open (a dimension closed over demand alone, flags closed for
the device-made contents only) is reported, never counted as closed.
"""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import caname  # noqa: E402
import caset  # noqa: E402
import caspec  # noqa: E402
import caspec27  # noqa: E402
import closure  # noqa: E402
import make_pack  # noqa: E402

CORPUS = os.path.join(HERE, '..', '..', 'precompile', 'corpus.log')
CA_NAMES = os.path.join(HERE, '..', '..', 'precompile', 'ca-names', '24B119.txt')
PROVEN = ('binary', 'manifest')


def context_of(tables: caspec27.Tables27, name: str) -> dict[str, object]:
    """Give the context fields of a name, by the table's dimension names."""
    s = caspec27.decode(tables, name)
    fields: dict[str, object] = {
        'attachments': (''.join(sorted(s.pflags & {'m', 'w'})), s.A),
        'S': s.S,
        'u': 'u' in s.flags,
        'F': tuple(img.F for img in s.images[: s.decoded]),
        'D': s.D,
    }
    fields.update({f: f in s.flags for f in closure.FLAGS})
    return fields


def differing(tables: caspec27.Tables27, a: str, b: str) -> list[str]:
    ca, cb = context_of(tables, a), context_of(tables, b)
    return [k for k in ca if ca[k] != cb[k]]


class Codec(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tables = caspec.Tables(os.path.join(HERE, 'qc-24B119.json')).v27

    def test_every_name_quartzcore_wrote_round_trips(self):
        written = [m[0] for m in self.tables.t['manifest'] if m[0].startswith('P')] + closure.demand('24B119')
        self.assertGreater(len(written), 900)
        for name in written:
            self.assertEqual(caname.encode(self.tables, caspec27.decode(self.tables, name)), name)

    def test_another_builds_names_are_not_strict(self):
        self.assertFalse(closure.strict(self.tables, 'PBGRAXmw_TpslA3Xhf'))  # a texture function 24B119 lacks
        self.assertFalse(closure.strict(self.tables, 'PBGRABsovXmw_TimgA3Xhfn_IsrcN3Oc3mtc4nl'))  # 21H461's opcodes
        self.assertTrue(closure.strict(self.tables, 'PBGRAXmw_TmuaA3S1Xhfu_Iara_Isqr'))


class Closure(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.qc = os.path.join(HERE, 'qc-24B119.json')
        cls.tables = caspec.Tables(cls.qc)
        cls.table = closure.context('24B119')
        with open(CA_NAMES) as f:
            names = f.read().split()
        tiers = make_pack.ca_states(make_pack.Path(cls.qc), make_pack.Path(CORPUS), names)
        lines = [line for _tier, made in tiers for line in made]
        cls.names = {caspec.corpus_fields(line)[0] for line in lines}
        cls.keys = {make_pack.state_key(line) for line in lines}
        named = caset.corpus_names(CORPUS, cls.tables) + [n for n in names if caset.makes(cls.tables, n)]
        cls.demanded = set(closure.demand('24B119'))
        cls.seeds = [
            n
            for n in dict.fromkeys(named + closure.demand('24B119'))
            if closure.strict(cls.tables.v27, n) and 'g' not in caspec27.decode(cls.tables.v27, n).flags
        ]

    def held(self, name: str) -> bool:
        if name in self.names:
            return True
        return make_pack.state_key(caspec.state_line(self.tables, name)) in self.keys

    def test_every_device_made_state_is_in_the_pack(self):
        missing = [n for n in closure.demand('24B119') if not self.held(n)]
        self.assertEqual(missing, [])

    def test_every_state_24b119_named_lies_inside_its_contents_closure(self):
        with open(CORPUS) as f:
            drawn = [caspec.corpus_fields(line)[0] for line in f if '24B119' in line.split(' | ', 1)[0]]
        manifest = [m[0] for m in self.tables.t['manifest'] if m[0].startswith('P')]
        named = [n for n in dict.fromkeys(drawn + manifest + closure.demand('24B119')) if n]
        checked = 0
        for name in named:
            if 'g' in caspec27.decode(self.tables.v27, name).flags:
                continue
            self.assertIn(name, closure.states(self.tables.v27, self.table, name), name)
            checked += 1
        self.assertGreater(checked, 900)

    def test_every_content_has_every_context_the_table_gives(self):
        for seed in self.seeds:
            for name in closure.states(self.tables.v27, self.table, seed, seed in self.demanded):
                if not self.held(name):
                    dims = differing(self.tables.v27, seed, name)
                    self.fail(f'{seed}: no state for its {", ".join(dims) or "own"} context ({name})')
        dims = self.table['dimensions']
        print(
            'open:',
            {k: d.get('unproven', d['values']) for k, d in dims.items() if d['proof'] not in PROVEN},
            'flags of contents no device made:',
            [k for k, d in dims.items() if d.get('seeds') == 'demand'],
            file=sys.stderr,
        )

    def test_no_content_is_put_on_fewer_attachments_than_its_fragment_writes(self):
        needs = self.table['dimensions']['attachments']['needs']
        checked = 0
        for seed in self.seeds:
            least = closure.least_attachments(self.table, self.tables.v27, caspec27.decode(self.tables.v27, seed))
            if not least:
                continue
            for name in closure.states(self.tables.v27, self.table, seed, seed in self.demanded):
                self.assertGreaterEqual(caspec27.decode(self.tables.v27, name).A, least, f'{seed}: {name}')
                checked += 1
        self.assertGreater(checked, 0, f'no seed of {sorted(needs)}')

    def test_a_build_without_a_table_keeps_its_tiers(self):
        t17 = caspec.Tables(os.path.join(HERE, 'qc-21H461.json'))
        self.assertIsNone(closure.context(t17.t['build']))
        made = caset.labels(t17, caset.corpus_names(CORPUS, t17))
        self.assertEqual(made['demand'], [])
        self.assertEqual(made['closure'], [])


if __name__ == '__main__':
    unittest.main()
