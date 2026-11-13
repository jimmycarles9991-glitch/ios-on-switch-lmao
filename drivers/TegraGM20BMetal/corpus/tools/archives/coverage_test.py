"""The raw pack holds every render state Apple's archives declare for the device (precompile/archives/<build>.tsv).

Per IPSW kind, each declared state is the pack's (its key is one the generator makes: Core Animation's from the corpus
and QuartzCore's tables, rawpack.py ca_states; every other framework's fed from the inventory, tools/gpu/rawpack.py
archive_lines, and kept by the generator's split), or one of the kinds below that raw does not take from the pack, each
with why. A new state in either inventory, or a pack that loses one, fails here.
"""

import unittest
from pathlib import Path

from drivers.TegraGM20BMetal.corpus.tools.archives.model import packable
from drivers.TegraGM20BMetal.raw.pack import rawpack
from tools.gpu import rawpack as steps

HERE = Path(__file__).absolute().parents[3]  # drivers/TegraGM20BMetal
KINDS = ('21H461', '24B119')
NOT_FROM_THE_PACK = {
    'compute': "the pack keys a kernel at its dispatch's threadgroup shape, which a descriptor never names",
    'tile': 'raw makes no tile render state',
    'tessellation': 'raw makes no tessellated render state (m2v refuses its patch draws)',
    'unspecialized': "a function lacks a constant its library requires: the archive does not record the caller's value",
    'creation': "not a vertex and a fragment function of one framework's default.metallib (or it links another "
    "library's) nor of one bundle's library linking none: the runtime makes it when created, never from the pack "
    '(RawMetalStates.mm pipelineFor)',
    'family': 'declared only for GPU families the raw device does not answer for (Apple4 and later)',
}
# QuartzCore's archive states whose key no QuartzCore descriptor on the device has, by why
QUARTZCORE_ELSEWHERE = {
    'generic': "a generic (fc_generic) state, which caset leaves out of 21H461's pack until a 17 boot asks for one",
    'path': "a path state: the archive leaves a target with blending off at Metal's default factors, which QuartzCore "
    'sets (the journal: c0=550:0:0:1:5:0:1:5:0), and the pack holds the state as QuartzCore makes it',
}


def quartzcore_elsewhere(line: str) -> str | None:
    if ' | v fixed_vert_lph_gen ' in line:
        return 'generic'
    if ' | v path_' in line and ':0:0:1:0:0:1:0:0' in line:
        return 'path'
    return None


def inventory(kind: str) -> list[tuple[str, str, str, str]]:
    rows = []
    for row in (HERE / f'precompile/archives/{kind}.tsv').read_text().splitlines():
        what, owner, _source, family, line = row.split('\t')
        rows.append((what, owner, family, line))
    return rows


class CorpusLines(unittest.TestCase):
    def test_the_pack_takes_every_framework_state_the_runtime_looks_up(self):
        lib = ' lib=/System/Library/PrivateFrameworks/{}.framework/default.metallib'
        vector = f'R ok=1 label=- samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0 | v a{lib.format("VectorKit")} '
        both = vector + f' | f b{lib.format("VectorKit")} #0:53:01 | '
        vertex_only = vector + ' | f -  | '
        foreign_link = both[:-2] + '| fp c lib=/Applications/X.app/default.metallib | '
        lines = steps.probe_lines('\n'.join((both, vertex_only, foreign_link)))
        self.assertEqual(lines, [both])
        self.assertTrue(packable(both))
        self.assertFalse(packable(vertex_only) or packable(foreign_link))


class Coverage(unittest.TestCase):
    def test_every_declared_render_state_is_the_packs_or_has_a_reason(self):
        for kind in KINDS:
            with self.subTest(kind=kind):
                rows = inventory(kind)
                fed = steps.archive_lines((HERE / f'precompile/archives/{kind}.tsv').read_text())
                self.assertTrue(all(rawpack.FRAMEWORK.search(line) for line in fed))
                packable_elsewhere = {
                    line
                    for what, owner, family, line in rows
                    if what == 'render' and family == 'device' and owner != 'QuartzCore.framework' and packable(line)
                }
                self.assertEqual(set(fed), packable_elsewhere, 'the generator feeds what the runtime looks up')
                pack = {rawpack.state_key(line) for line in fed}
                tables = HERE / f'raw/pack/ca/qc-{kind}.json'
                names = HERE / f'precompile/ca-names/{kind}.txt'
                named = names.read_text().split() if names.exists() else []
                for _tier, lines in rawpack.ca_states(tables, HERE / 'precompile/corpus.log', named):
                    pack |= {rawpack.state_key(line) for line in lines}
                left: dict[str, int] = {}
                for what, owner, family, line in rows:
                    reason = what if what in NOT_FROM_THE_PACK else None
                    if not reason and family != 'device':
                        reason = 'family'
                    if not reason and rawpack.state_key(line) not in pack:
                        reason = quartzcore_elsewhere(line) if owner == 'QuartzCore.framework' else None
                        if reason is None and not packable(line):
                            reason = 'creation'
                        self.assertIsNotNone(reason, f'{kind}: {owner} declares a state the pack lacks: {line[:300]}')
                    if reason:
                        left[reason] = left.get(reason, 0) + 1
                self.assertGreater(len(rows) - sum(left.values()), 0)
                for reason in left:
                    self.assertIn(reason, NOT_FROM_THE_PACK | QUARTZCORE_ELSEWHERE)


if __name__ == '__main__':
    unittest.main()
