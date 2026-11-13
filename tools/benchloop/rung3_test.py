"""rung3.py: each bench table loads, and its formula is the program's arithmetic on premultiplied colours."""

import random
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.benchloop import blend, formula, rung3

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'drivers/TegraGM20BMetal/raw/pack/iterated'))
import emu
import table


def premultiplied(rng: random.Random) -> tuple[float, float, float, float]:
    a = rng.choice((0.0, 1.0, rng.random()))
    return (*(a * rng.random() for _ in range(3)), a)


class Tables(unittest.TestCase):
    def test_each_formula_is_its_program(self):
        rng = random.Random(3)
        written = [name for name in table.PROGRAMS if rung3.bench_table(name)]
        self.assertGreaterEqual(len(written), 15)
        # the bench's own colours (not premultiplied), then premultiplied ones as Core Animation's are
        colours = blend.colours(64, 1)
        pairs = [(s, d) for s in colours for d in colours[:16]]
        pairs += [(premultiplied(rng), premultiplied(rng)) for _ in range(200)]
        for name in written:
            t = blend.Table(rung3.bench_table(name))
            for src, dst in pairs:
                want = formula.blend(t.rgb, t.a, src, dst)
                got = emu.run(table.PROGRAMS[name], list(src), list(dst))
                for g, w in zip(got, want, strict=True):  # as an 8-bit target stores them
                    self.assertAlmostEqual(min(max(g, 0), 1), min(max(w, 0), 1), delta=1e-3, msg=f'{name} {src} {dst}')

    def test_the_per_target_blend_goes_back_on(self):
        t = blend.Table(rung3.bench_table('mpy'))
        methods, restore = t.methods(None)
        self.assertIn([rung3.PER_TARGET, 0], methods)
        self.assertIn([rung3.PER_TARGET, 1], restore)
        self.assertEqual(restore[0], [blend.SET_ITERATED_BLEND, 0])
        self.assertNotIn([blend.SET_ITERATED_BLEND_PASS, 0], restore)


if __name__ == '__main__':
    unittest.main()
