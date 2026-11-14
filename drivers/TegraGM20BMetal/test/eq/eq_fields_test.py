"""eq_fields.py's frame on a log made by hand: a strip raw drew a triangle at a time folds back into one draw."""

import tempfile
import unittest
from pathlib import Path

import eq_fields


def draw(first: int, barriers: str, front: str, extra: str = '') -> str:
    return (
        f'DRAW first={first} count=3 begin=00000000 barriers={barriers}\n'
        f'  regs={front}{extra}\n'
        f'  R 300=3 191c={front}{extra}\n'
        '  rt0=R1 64x64 fmt=cf mem=1000 pitch=0\n'
        f'  vb0 stride=48 data={first}:90\n'
        '  stage5 prog=5 regs=24 group=4\n'
        f'    rootwords draw: {first} 0 0 push: 0\n'
    )


def frame(text: str) -> eq_fields.Frame:
    with tempfile.TemporaryDirectory() as tmp:
        log = Path(tmp) / 'raw.log'
        log.write_text(text)
        return eq_fields.frame(str(log))


class Runs(unittest.TestCase):
    def test_a_strips_runs_fold_across_submissions_whatever_each_sets_of_its_own(self):
        f = frame(
            'SUBMIT 1\n'
            + draw(0, 'PSB,TEXINV', '900')
            + 'SUBMIT 2\n'
            + draw(1, 'PSB,SPLIT,TEXINV', '901')
            + 'SUBMIT 3\n'
        )
        self.assertEqual(len(f.draws), 1)
        self.assertEqual(f.draws[0].fields['draw'].split()[1:3], ['0', '4'])
        self.assertEqual(f.draws[0].registers['191c'], '900')

    def test_a_run_of_other_state_stays_a_draw(self):
        f = frame(
            'SUBMIT 1\n' + draw(0, 'PSB,TEXINV', '900') + draw(1, 'PSB,SPLIT,TEXINV', '901', ' 1a00=1') + 'SUBMIT 2\n'
        )
        self.assertEqual(len(f.draws), 2)

    def test_a_draw_that_is_no_run_stays_a_draw(self):
        f = frame('SUBMIT 1\n' + draw(0, 'PSB,TEXINV', '900') + draw(1, 'PSB,TEXINV', '901') + 'SUBMIT 2\n')
        self.assertEqual(len(f.draws), 2)


if __name__ == '__main__':
    unittest.main()
