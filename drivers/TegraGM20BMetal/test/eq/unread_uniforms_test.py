"""eq's unread class, proven on the states it was ruled for: a fragment uniform buffer raw's storage form leaves unread.

Each state of testdata/unread/unread.tsv draws in raw's storage form, whose program blends in the interlock and reads
none of the listed uniform buffers, so raw keeps the set-0 upload its earlier draw made though their bindings moved. The
state's barrier form, compiled from the same function, reads them. Run on tools/gpu/maxwell's interpreter, that program
gives the same values of every component its ROP blend reads of the source whatever those buffers hold, and its blend
reads the source only through those components: ZERO times the source plus the destination times ONE, the source
alpha or one less it. What the buffers feed is a colour the blend drops, so a program that leaves them unread is right.
"""

import hashlib
import random
import struct
import unittest
from collections.abc import Callable
from pathlib import Path

from tools.gpu.maxwell import isa
from tools.gpu.maxwell.interp import Inputs
from tools.gpu.maxwell.quad import LANES, run_quad

DATA = Path(__file__).with_name('testdata') / 'unread'
ZERO, ONE, SRC_ALPHA, ONE_MINUS_SRC_ALPHA = 0x4000, 0x4001, 0x4302, 0x4303
UBO, SET = 5, 3
SEEDS = 6


def f32(x: float) -> int:
    return struct.unpack('<I', struct.pack('<f', x))[0]


def f16(x: float) -> int:
    return struct.unpack('<H', struct.pack('<e', x))[0]


def uniform_words(seed: int) -> list[int]:
    """Give a buffer's words: floats and half pairs in (0, 1), as Core Animation's uniforms hold."""
    rng = random.Random(seed)
    return [
        f16(rng.uniform(0.05, 0.95)) | f16(rng.uniform(0.05, 0.95)) << 16 if i % 2 else f32(rng.uniform(0.05, 0.95))
        for i in range(128)
    ]


def texel(key: tuple) -> list[int]:
    """Give a fetch's result as a function of its whole key (handle, coordinates, level), as interp.py's model asks."""
    d = hashlib.sha256(repr(key).encode()).digest()
    return [f32(0.05 + 0.9 * d[c] / 255) for c in range(4)]


def set_word(offset: int) -> int:
    """Give a word of set 0: each 16-byte descriptor an address and a range."""
    return {0: 0x40000 + 16 * offset, 8: 0x100}.get(offset % 16, 0)


def blend_reads(row: dict) -> set[int]:
    """Give the source components the target's ROP blend reads: none or its alpha (the blend's factors decide)."""
    src_rgb, dst_rgb, src_alpha, dst_alpha = row['blend']
    assert src_rgb == src_alpha == ZERO, f'{row["key"]}: the blend takes the source itself'
    reads = set()
    for dst in (dst_rgb, dst_alpha):
        assert dst in {ONE, SRC_ALPHA, ONE_MINUS_SRC_ALPHA}, f'{row["key"]}: dst factor {dst:x}'
        if dst != ONE:
            reads.add(3)
    return reads


def output_register(omap: int, target: int, component: int) -> int:
    """Give the register holding a colour component: the omap's enabled components before it, in order."""
    return (omap & ((1 << (4 * target + component)) - 1)).bit_count()


def banks_read(code: bytes) -> set[int]:
    """Give the constant buffer banks the program reads: _cbuf operands' and LDC's."""
    banks = set()
    for w in isa.instructions(code):
        name = isa.name(w)
        if name.endswith('_cbuf'):
            banks.add(isa.field(w, 34, 5))
        elif name.startswith('LDC'):
            banks.add(isa.field(w, 36, 5))
    return banks


def rows() -> list[dict]:
    out = []
    for line in (DATA / 'unread.tsv').read_text().splitlines():
        if line.startswith('#'):
            continue
        key, kind, label, types, bound, banks, omap, target, blend = line.split('\t')
        out.append(
            {
                'key': key,
                'kind': kind,
                'label': label,
                'types': [int(t) for t in types.split(',')],
                'bound': int(bound),
                'banks': {int(b) for b in banks.split(',')},
                'omap': int(omap, 16),
                'target': int(target),
                'blend': [int(b, 16) for b in blend.split(',')],
            }
        )
    return out


def run(row: dict, code: bytes, seed: int, unread: list[int]) -> list[tuple]:
    """Run a quad of the program: inputs of `seed`, the unread buffers holding `unread`."""
    fixed = uniform_words(1000 + seed)

    def cbuf(bank: int, offset: int) -> int:
        kind = row['types'][bank] & 7 if bank < len(row['types']) else 0
        if kind == UBO:
            return (unread if bank in row['banks'] else fixed)[offset // 4 % len(fixed)]
        return set_word(offset) if kind == SET else 0

    def attribute(lane: int) -> Callable[[int], int]:
        return lambda address: f32(0.1 + (address * 37 + lane * 11 + seed * 5) % 80 / 100)

    lanes = [Inputs(attribute(lane), cbuf, texel, bound_cbuf=row['bound'] - 1) for lane in range(LANES)]
    return run_quad(code, lanes, 16)


class UnreadUniforms(unittest.TestCase):
    def test_every_state_has_a_program_and_a_buffer_it_reads(self):
        table = rows()
        self.assertEqual(len(table), 7)
        for row in table:
            code = isa.from_hex_dump((DATA / f'{row["key"]}.hex').read_text())
            self.assertLessEqual(row['banks'], banks_read(code), row['label'])

    def test_what_the_blend_reads_of_the_source_does_not_depend_on_the_unread_buffers(self):
        for row in rows():
            code = isa.from_hex_dump((DATA / f'{row["key"]}.hex').read_text())
            registers = [output_register(row['omap'], row['target'], c) for c in sorted(blend_reads(row))]
            for seed in range(SEEDS):
                first = run(row, code, seed, uniform_words(2 * seed))
                second = run(row, code, seed, uniform_words(2 * seed + 1))
                for lane in range(LANES):
                    self.assertFalse(first[lane][1] or second[lane][1], f'{row["label"]}: killed')
                    self.assertEqual(
                        [first[lane][0][r] for r in registers],
                        [second[lane][0][r] for r in registers],
                        f'{row["label"]} seed {seed} lane {lane}',
                    )


if __name__ == '__main__':
    unittest.main()
