"""rb/rbclosure.py over the precompile corpus's RenderBox states and those 24B119 devices made (demand-24B119.json).

A state is compared by the pack generator's own key of its line (make_pack.state_key), as the device logged it.
"""

import json
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path[:0] = [str(HERE), str(HERE.parent)]

import make_pack  # noqa: E402
import rbclosure  # noqa: E402
import rbspec  # noqa: E402

CORPUS = HERE.parents[1] / 'precompile' / 'corpus.log'
WIDGETS = 'WidgetRenderer_Default'


def corpus(build: str) -> list[str]:
    """Give the corpus's render lines a build's pack takes, as tools/gpu/make_pack.py own_build gives them."""
    with open(CORPUS) as f:
        drawn = [line.rstrip('\n') for line in f if line.startswith('R ')]
    tagged = [line for line in drawn if make_pack.OS_BUILD.search(line.split(' | ', 1)[0])]
    return [make_pack.OS_BUILD.sub('', line, count=1) for line in tagged if make_pack.drawn_here(line, build)]


def keys(lines: list[str]) -> set[str]:
    return {make_pack.state_key(line) for line in lines}


class RenderBox27(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        with open(HERE / 'rb-24B119.json') as f:
            cls.digest = json.load(f)['quartzcore_metallib_sha256']
        cls.corpus = corpus('24B119')
        cls.tables_and_closure = rbclosure.for_quartzcore(HERE, cls.digest)
        with open(HERE / 'demand-24B119.json') as f:
            cls.demand = json.load(f)['states']

    @property
    def tables(self) -> rbspec.Tables:
        return self.found()[0]

    @property
    def closure(self) -> rbclosure.Closure:
        return self.found()[1]

    def found(self) -> tuple[rbspec.Tables, rbclosure.Closure]:
        self.assertIsNotNone(self.tables_and_closure, 'rb-24B119.json has no closure')
        return self.tables_and_closure

    def made(self, lines: list[str]) -> list[str]:
        return rbclosure.made(HERE, self.digest, lines, make_pack.state_key)

    def test_every_device_made_renderbox_state_is_in_the_pack(self):
        held = keys(self.corpus) | keys(self.made(self.corpus))
        self.assertEqual([d['key'] for d in self.demand if d['key'] not in held], [])

    def test_the_closure_makes_the_widget_states_boot_b_lacked_from_the_corpus_without_them(self):
        lacked = {d['key'] for d in self.demand if WIDGETS in d['processes'] and d['boots'] == ['boot B']}
        self.assertTrue(lacked)
        seeds = [line for line in self.corpus if make_pack.state_key(line) not in lacked]
        self.assertEqual(sorted(lacked - keys(self.made(seeds))), [])

    def test_the_closure_holds_no_state_twice_and_none_the_corpus_holds(self):
        made = self.made(self.corpus)
        self.assertEqual(len(keys(made)), len(made))
        self.assertFalse(keys(made) & keys(self.corpus))

    def test_a_vertex_function_carries_its_fragments_block(self):
        for line in self.made(self.corpus):
            blocks = {part[:32] for part in line.split(rbclosure.BLOCK)[1:]}
            self.assertEqual(len(blocks), 1, line)

    def underived(self, lines: list[str]) -> list[str]:
        """Give the RenderBox lines whose target rule, word 3 or written blend is not what RenderBox derives."""
        out = []
        for line in lines:
            s = rbspec.decode(line) if rbspec.renderbox_line(line) else None
            if not s or s.function in self.closure.exempt:
                continue
            state = (list(s.words), s.colours)
            if (
                not rbclosure.keeps_target_rule(self.closure, *state)
                or rbclosure.placed(self.tables, self.closure, *state) != state
            ):
                out.append(line[:240])
        return out

    def test_every_state_devices_drew_is_what_renderbox_derives_of_its_target_shape_and_mode(self):
        self.assertEqual(self.underived(self.corpus), [])

    def test_every_closure_state_is_what_renderbox_derives_of_its_target_shape_and_mode(self):
        self.assertEqual(self.underived(self.made(self.corpus)), [])

    def test_an_alpha_only_state_moves_only_among_alpha_only_targets(self):
        line = next(
            line
            for line in self.corpus
            if rbspec.renderbox_line(line)
            and rbspec.decode(line)
            and rbspec.decode(line).field(self.tables, 'alpha_only')
        )
        targets = {rbspec.decode(state).colours[0][0] for state in rbclosure.states_of(self.tables, self.closure, line)}
        self.assertLessEqual(targets, set(self.closure.alpha_targets))

    def test_make_pack_places_closure_states_in_tier_1_and_keeps_corpus_in_tier_0(self):
        made = make_pack.renderbox_closure(self.digest, self.corpus)
        self.assertGreater(len(made), 30_000)
        for line in self.corpus:
            if rbspec.renderbox_line(line):
                self.assertNotIn(' tier=1 ', line)
        tagged = [make_pack.cold_line(line) for line in made[:64]]
        for orig, cold in zip(made[:64], tagged):
            self.assertIn(' tier=1 samples=', cold)
            self.assertEqual(make_pack.state_key(cold), make_pack.state_key(orig))


class RenderBox17(unittest.TestCase):
    def test_a_build_without_a_closure_makes_none(self):
        with open(HERE / 'rb-21H461.json') as f:
            digest = json.load(f)['quartzcore_metallib_sha256']
        self.assertIsNone(rbclosure.for_quartzcore(HERE, digest))
        self.assertEqual(rbclosure.made(HERE, digest, corpus('21H461'), make_pack.state_key), [])


if __name__ == '__main__':
    unittest.main()
