#!/usr/bin/env python3
"""closure.py and caname.py: the pack holds every context of every known Core Animation content, by QuartzCore's names.

The name codec is held to the names QuartzCore itself wrote on both 24B119 and 21H461: each build's offline manifest
and the states its devices made (demand-<build>.json). Each build's table rules are held to the states that build named:
each lies inside its own content's closure. The closure is held to the pack's own corpus lines (make_pack.ca_states):
each seed's content under each context value the table gives has a line, and a missing one names its dimension.
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
ALL_FILTERS = ('nr', 'bx', 'b4', 'b8', 'mb', 'll', '77', 'as', 'la')


def context_of(tables: closure.TablesLike, table: dict, name: str) -> dict[str, object]:
    """Give the context fields of a name, by the table's dimension names."""
    inner = closure._unwrap(tables)
    s = closure._decode(inner, name)
    fields: dict[str, object] = {
        'attachments': (''.join(sorted(s.pflags & {'m', 'w'})), s.A),
        'S': s.S,
        'u': s.flag('u'),
        'F': tuple(img.F for img in closure._images(inner, s)),
        'D': s.D,
    }
    fields.update({f: s.flag(f) for f in closure._table_flags(table)})
    return fields


def differing(tables: closure.TablesLike, table: dict, a: str, b: str) -> list[str]:
    ca, cb = context_of(tables, table, a), context_of(tables, table, b)
    return [k for k in ca if ca[k] != cb[k]]


class Codec(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tables27 = caspec.Tables(os.path.join(HERE, 'qc-24B119.json')).v27
        cls.tables17 = caspec.Tables(os.path.join(HERE, 'qc-21H461.json'))

    def test_every_name_quartzcore_27_wrote_round_trips(self):
        written = [m[0] for m in self.tables27.t['manifest'] if m[0].startswith('P')] + closure.demand('24B119')
        self.assertGreater(len(written), 900)
        for name in written:
            self.assertEqual(caname.encode(self.tables27, caspec27.decode(self.tables27, name)), name)

    def test_every_name_quartzcore_17_wrote_round_trips(self):
        written = [m[0] for m in self.tables17.t['manifest'] if m[0].startswith('P')] + closure.demand('21H461')
        self.assertGreater(len(written), 1000)
        for name in written:
            self.assertEqual(caname.encode(self.tables17, caspec.decode(self.tables17, name)), name)

    def test_another_builds_names_are_not_strict(self):
        self.assertFalse(closure.strict(self.tables27, 'PBGRAXmw_TpslA3Xhf'))  # a texture function 24B119 lacks
        self.assertFalse(closure.strict(self.tables27, 'PBGRABsovXmw_TimgA3Xhfn_IsrcN3Oc3mtc4nl'))  # 21H461's opcodes
        self.assertTrue(closure.strict(self.tables27, 'PBGRAXmw_TmuaA3S1Xhfu_Iara_Isqr'))
        self.assertFalse(closure.strict(self.tables17, 'PBGRAXm_TimgA2Xhfv_Isrc'))  # 24B119's v flag
        self.assertFalse(closure.strict(self.tables17, 'PBGRAXm_TimgBdlcA2Xhf_Isrc'))  # 24B119's blend code
        self.assertFalse(closure.strict(self.tables17, 'PBGRABsovXmw_TimgA3Xhf_IsrcN3Oc3mtc4nlnlnl'))  # 6 opcodes
        self.assertTrue(closure.strict(self.tables17, 'PBGRAXmw_TpslA3Xhf'))


class _ClosureBase(unittest.TestCase):
    BUILD: str
    QC_FILE: str
    CA_NAMES_FILE: str | None = None

    @classmethod
    def setUpClass(cls) -> None:
        if cls is _ClosureBase:
            raise unittest.SkipTest('base class')
        cls.qc = os.path.join(HERE, cls.QC_FILE)
        cls.tables = caspec.Tables(cls.qc)
        cls.inner = closure._unwrap(cls.tables)
        cls.table = closure.context(cls.BUILD)
        names = []
        if cls.CA_NAMES_FILE:
            with open(cls.CA_NAMES_FILE) as f:
                names = f.read().split()
        tiers = make_pack.ca_states(make_pack.Path(cls.qc), make_pack.Path(CORPUS), names)
        lines = [line for _tier, made in tiers for line in made]
        cls.names = {caspec.corpus_fields(line)[0] for line in lines}
        cls.keys = {make_pack.state_key(line) for line in lines}
        named = caset.corpus_names(CORPUS, cls.tables) + [n for n in names if caset.makes(cls.tables, n)]
        cls.demanded = set(closure.demand(cls.BUILD))
        cls.seeds = [
            n
            for n in dict.fromkeys(named + closure.demand(cls.BUILD))
            if closure.strict(cls.inner, n) and not closure._decode(cls.inner, n).flag('g')
        ]

    def held(self, name: str) -> bool:
        if name in self.names:
            return True
        return make_pack.state_key(caspec.state_line(self.tables, name)) in self.keys

    def test_every_device_made_state_is_in_the_pack(self):
        missing = [n for n in closure.demand(self.BUILD) if not self.held(n)]
        self.assertEqual(missing, [])

    def test_every_state_this_build_named_lies_inside_its_contents_closure(self):
        with open(CORPUS) as f:
            drawn = [
                caspec.corpus_fields(line)[0]
                for line in f
                if self.BUILD in line.split(' | ', 1)[0] and caspec.corpus_fields(line)[0]
            ]
        drawn = [n for n in drawn if closure.strict(self.inner, n)]
        manifest = [m[0] for m in self.tables.t['manifest'] if m[0].startswith('P')]
        named = [n for n in dict.fromkeys(drawn + manifest + closure.demand(self.BUILD)) if n]
        checked = 0
        for name in named:
            if closure._decode(self.inner, name).flag('g'):
                continue
            self.assertIn(name, closure.states(self.inner, self.table, name), name)
            checked += 1
        self.assertGreater(checked, 900)

    def test_every_content_has_every_context_the_table_gives(self):
        for seed in self.seeds:
            for name in closure.states(self.inner, self.table, seed, seed in self.demanded):
                if not self.held(name):
                    dims = differing(self.inner, self.table, seed, name)
                    self.fail(f'{seed}: no state for its {", ".join(dims) or "own"} context ({name})')
        dims = self.table['dimensions']
        print(
            f'{self.BUILD} open:',
            {k: d.get('unproven', d['values']) for k, d in dims.items() if d['proof'] not in PROVEN},
            'flags of contents no device made:',
            [k for k, d in dims.items() if d.get('seeds') == 'demand'],
            file=sys.stderr,
        )

    def test_no_content_is_put_on_fewer_attachments_than_its_fragment_writes(self):
        needs = self.table['dimensions']['attachments']['needs']
        checked = 0
        for seed in self.seeds:
            least = closure.least_attachments(self.table, self.inner, closure._decode(self.inner, seed))
            if not least:
                continue
            for name in closure.states(self.inner, self.table, seed, seed in self.demanded):
                self.assertGreaterEqual(closure._decode(self.inner, name).A, least, f'{seed}: {name}')
                checked += 1
        self.assertGreater(checked, 0, f'no seed of {sorted(needs)}')

    def test_all_nine_filters_are_closed_for_textured_images_only(self):
        f_dim = self.table['dimensions']['F']
        self.assertIn(f_dim['proof'], PROVEN)
        self.assertNotIn('unproven', f_dim)
        self.assertEqual(tuple(f_dim['values']), ALL_FILTERS)
        tex_seed = 'PBGRABsovXm_TimgA2Xhf_Isrc'
        tex_states = closure.states(self.inner, self.table, tex_seed, True)
        seen_f = {closure._decode(self.inner, n).images[0].F for n in tex_states}
        want_f = {self.inner.dec['TextureFilter'][code] for code in ALL_FILTERS}
        self.assertEqual(seen_f, want_f)
        for code in ALL_FILTERS:
            expected = 'PBGRABsovXm_TimgA2Xhf_Isrc' + ('' if code == 'nr' else f'F{code}')
            self.assertTrue(self.held(expected), f'missing {expected}')
        proc_seed = 'PBGRABsovXm_TimgA2Xhf_Isqr'
        proc_states = closure.states(self.inner, self.table, proc_seed, True)
        for n in proc_states:
            s = closure._decode(self.inner, n)
            self.assertEqual(s.images[0].F, 0, f'procedural image got non-nr filter: {n}')
            self.assertFalse(s.flag('p'), f'procedural image got premultiply flag p: {n}')
        mem_seed = 'PBGRABsovXm_TcimA2Xhf_IsrcM1'
        for n in closure.states(self.inner, self.table, mem_seed, True):
            s = closure._decode(self.inner, n)
            self.assertEqual(s.images[0].F, 0, f'memoryless image got non-nr filter: {n}')
            self.assertFalse(s.flag('p'), f'memoryless image got premultiply flag p: {n}')


class Closure24B119(_ClosureBase):
    BUILD = '24B119'
    QC_FILE = 'qc-24B119.json'
    CA_NAMES_FILE = CA_NAMES

    def test_attachment_one_sets_unbounded_flag_on_24b119(self):
        made = closure.states(self.inner, self.table, 'PBGRABsovXm_TimgA2Xhf_Isrc', False)
        s1 = [closure._decode(self.inner, n) for n in made if closure._decode(self.inner, n).S == 1]
        self.assertTrue(s1)
        self.assertTrue(all(s.flag('u') for s in s1))


class Closure21H461(_ClosureBase):
    BUILD = '21H461'
    QC_FILE = 'qc-21H461.json'

    def test_ios17_omits_v_flag_and_keeps_bgra_attachment_one_bounded(self):
        self.assertNotIn('v', self.table['dimensions'])
        made = closure.states(self.inner, self.table, 'PBGRABsovXm_TimgA2Xhf_Isrc', True)
        self.assertTrue(made)
        for n in made:
            self.assertNotIn('v', n.split('_')[1], n)
        s1 = [closure._decode(self.inner, n) for n in made if closure._decode(self.inner, n).S == 1]
        self.assertTrue(s1)
        self.assertTrue(all(not s.flag('u') for s in s1))

    def test_unknown_build_has_no_context_or_demand(self):
        self.assertIsNone(closure.context('99Z999'))
        self.assertEqual(closure.demand('99Z999'), [])


if __name__ == '__main__':
    unittest.main()
