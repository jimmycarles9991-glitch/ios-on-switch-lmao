"""eq_fields.py's frame on a log made by hand: a split draw's BEGINs fold back into one draw."""

import tempfile
import unittest
from pathlib import Path

import eq_fields

# BEGIN words by SPLIT_MODE (30:29) over a triangle list's op: one normal draw, a split draw's first BEGIN, its later
# ones (primitive and instance IDs unchanged) and its last
UNCHANGED_IDS = 2 << 26 | 1 << 24
NORMAL, OPENS, CONTINUES, CLOSES = 0x4, 0x4 | 1 << 29, 0x4 | 2 << 29 | UNCHANGED_IDS, 0x4 | 3 << 29 | UNCHANGED_IDS


def draw(first: int, count: int, begin: int, extra: str = '') -> str:
    return (
        f'DRAW first={first} count={count} begin={begin:08x} barriers=PSB,TEXINV\n'
        f'  regs=900{extra}\n'
        f'  R 300=3 191c=900{extra}\n'
        '  rt0=R1 64x64 fmt=cf mem=1000 pitch=0\n'
        f'  vb0 stride=48 data={first + count}:90\n'
        '  stage5 prog=5 regs=24 group=4\n'
        '    rootwords draw: 0 0 0 push: 0\n'
    )


def indexed(first: int, count: int, begin: int, idx: str) -> str:
    return (
        f'DRAW-INDEXED first=0 count={first + count} begin={begin:08x} barriers=PSB,TEXINV\n'
        '  R 300=3\n'
        f'  vb0 stride=48 data={first + count}:90\n'
        f'  ib size=2 first={first} count={count} idx={idx} restart=ffff end=0\n'
    )


def frame(text: str) -> eq_fields.Frame:
    with tempfile.TemporaryDirectory() as tmp:
        log = Path(tmp) / 'gm20b.log'
        log.write_text(text)
        return eq_fields.frame(str(log))


class Split(unittest.TestCase):
    def test_a_split_draws_begins_fold_into_one_normal_draw(self):
        f = frame(
            'SUBMIT 1\n'
            + draw(0, 3, OPENS)
            + draw(3, 3, CONTINUES)
            + draw(6, 3, CONTINUES)
            + draw(9, 0, CLOSES)
            + 'SUBMIT 2\n'
        )
        self.assertEqual(len(f.draws), 1)
        self.assertEqual(f.draws[0].fields['draw'], 'DRAW 0 9 00000004')
        self.assertEqual(f.draws[0].fields['vb0'], 'vb0 stride=48 data=9:90')
        self.assertEqual(f.draws[0].barriers, {'PSB', 'TEXINV'})

    def test_an_indexed_split_draws_indices_join(self):
        f = frame('SUBMIT 1\n' + indexed(0, 3, OPENS, '0,1,2') + indexed(3, 3, CLOSES, '2,1,3') + 'SUBMIT 2\n')
        self.assertEqual(len(f.draws), 1)
        self.assertEqual(f.draws[0].fields['ib'], 'ib size=2 first=0 count=6 idx=0,1,2,2,1,3 restart=ffff end=0')

    def test_normal_begins_stay_draws(self):
        f = frame('SUBMIT 1\n' + draw(0, 3, NORMAL) + draw(3, 3, NORMAL) + 'SUBMIT 2\n')
        self.assertEqual(len(f.draws), 2)

    def test_a_begin_of_other_state_or_vertices_stays_a_draw(self):
        other = frame('SUBMIT 1\n' + draw(0, 3, OPENS) + draw(3, 3, CLOSES, ' 1a00=1') + 'SUBMIT 2\n')
        self.assertEqual(len(other.draws), 2)
        apart = frame('SUBMIT 1\n' + draw(0, 3, OPENS) + draw(4, 3, CLOSES) + 'SUBMIT 2\n')
        self.assertEqual(len(apart.draws), 2)


if __name__ == '__main__':
    unittest.main()
