#!/usr/bin/env python3
"""The storage form's in-shader blend run on the host: each program's stored texel against the blend model, bit for bit.

usage: blendcheck.py <dump dir | states.json> [--invocations N] [--seed S]
       blendcheck.py --corpus      (the corpus lines of the states testdata/rop_blend holds)

A dump is directcheck's DIRECTCHECK_DUMP of a state of testdata/rop_blend/rop_blend.metal, whose fragment fetches the
color it writes while the pipeline blends it: the source is a texture sample (the second source another), so each run
sets both, the destination is what the surface load gives, and the blend color the push words the record names. The
interpreter (quad.py) runs the program; its surface store's lanes must be the model's (m2v lower/fragment/rop_blend.rs):
normalized sources clamped to [0, 1] (NaN to 0), the destination code's exact value, each term a product rounded to
f32 (zero and one factors exact), the sum rounded to f32, min and max as FMNMX gives them, f32 denormals flushed as the
shader's ALU flushes them. The store's conversion to the format is the model's assumption, not checked here.

Regenerate the dumps: compile rop_blend.metal (xcrun -sdk iphoneos metal ... && metallib), then
PROBE_LABEL=ropcheck. DIRECTCHECK_DUMP=<dir> directcheck <nvk> <cache> <metallib> <(blendcheck.py --corpus) <pack>.
"""

import argparse
import json
import math
import random
import re
import struct
import sys
from fractions import Fraction
from pathlib import Path

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from tools.gpu.maxwell import isa
from tools.gpu.maxwell.execute import fminmax
from tools.gpu.maxwell.floats import F16, F32, arith, convert, encode, flush
from tools.gpu.maxwell.interp import Inputs
from tools.gpu.maxwell.quad import LANES, run_quad

ONE, ZERO = 0x3F800000, 0
SPECIALS = [
    0x7FC00000,
    0xFFC12345,
    0x7F800000,
    0xFF800000,
    0x80000000,
    0x00000001,
    0x807FFFFF,
    0x477FE000,
    ONE,
    0x00800000,
    0x3F000000,
    0xBF000000,
    0x3C000000,
]
# Metal pixel format: (components, normalized bits per component or None for float, half)
FORMATS = {
    80: (4, (8, 8, 8, 8), False),
    70: (4, (8, 8, 8, 8), False),
    10: (1, (8,), False),
    90: (4, (10, 10, 10, 2), False),
    115: (4, None, True),
    65: (2, None, True),
    55: (1, None, False),
    43: (4, (5, 5, 5, 1), False),
}
# BGR5A1Unorm, which the storage form views in R16_UINT (m2v lower/fragment/fetch/packed.rs): each channel's offset in
# the texel, red first. Its program stores the texel's word, of each blended channel's nearest code.
PACKED = {43: (10, 5, 0, 15)}
SOURCE_SAMPLE, SECOND_SAMPLE = 0x3E800000, 0x3F400000  # the sample coordinates' x: 0.25 and 0.75
PUSH_BASE = 0x700

FACTOR = {
    '0': 0,
    '1': 1,
    'sc': 2,
    '1-sc': 3,
    'sa': 4,
    '1-sa': 5,
    'dc': 6,
    '1-dc': 7,
    'da': 8,
    '1-da': 9,
    'sat': 10,
    'k': 11,
    '1-k': 12,
    'ka': 13,
    '1-ka': 14,
    's1c': 15,
    '1-s1c': 16,
    's1a': 17,
    '1-s1a': 18,
}
OP = {'add': 0, 'sub': 1, 'rsub': 2, 'min': 3, 'max': 4}
BLENDS = {
    'sov': ('1', '1-sa', 'add', '1', '1-sa', 'add'),
    'ca_da': ('da', '1-sa', 'add', 'da', '1-sa', 'add'),
    'ca_inv': ('1-da', '1-sa', 'add', '1-da', '1-sa', 'add'),
    'sub': ('1', '1', 'sub', '1', '1', 'sub'),
    'rsub': ('1', '1', 'rsub', '0', '1', 'rsub'),
    'min': ('1', '1', 'min', '1', '1', 'min'),
    'max': ('0', '0', 'max', '1', '1', 'max'),
    'const': ('k', '1-k', 'add', 'ka', '1-ka', 'add'),
    'const2': ('1-dc', '1-sc', 'add', '1', '1-sa', 'add'),
    'sep': ('dc', '0', 'add', 'da', '0', 'add'),
    'sat': ('sat', '1', 'add', 'sat', '1', 'add'),
    'mul': ('sc', 'dc', 'add', 'sa', 'da', 'add'),
}
DUAL = {'dual': ('s1c', '1-s1a', 'add', 's1a', '1-s1c', 'add')}
# Metal's write mask: red is bit 3
STATES = (
    [(80, b, 15) for b in BLENDS]
    + [(80, 'sov', 14), (80, 'ca_da', 8), (80, 'dual', 15)]
    + [(70, b, 15) for b in ('sov', 'const', 'mul')]
    + [(10, b, 15) for b in ('sov', 'sub', 'mul')]
    + [(90, b, 15) for b in ('sov', 'ca_inv')]
    + [(115, b, 15) for b in BLENDS]
    + [(115, 'sov', 13), (115, 'dual', 15)]
    + [(65, b, 15) for b in ('sov', 'min', 'const')]
    + [(65, 'sov', 4)]
    + [(55, b, 15) for b in ('sov', 'sub', 'max', 'mul')]
    + [(43, b, 15) for b in ('sov', 'const', 'mul', 'sub')]
    + [(43, 'sov', 14), (43, 'ca_da', 9)]
)
# Functions that declare the color they write but never read it: the device keeps their blend, no storage form.
UNREAD = [(80, 'sov', 15), (115, 'mul', 15)]
# A function whose output's green is the constant 0 (rop_frag_const) on float targets, where a product of it with an
# infinity or a NaN, and its sum with a -0, are the model's only if the compiler keeps them
CONSTANT = [(115, 'sep', 15), (115, 'sov', 15), (65, 'sep', 15)]
# Unblended writes of a packed format, which its storage form packs as Metal writes the output: through a half first
UNBLENDED = [(43, 15), (43, 14)]


def corpus() -> list[str]:
    lines = []
    for fmt, name, mask in STATES:
        f = [FACTOR[x] if x in FACTOR else OP[x] for x in {**BLENDS, **DUAL}[name]]
        fragment = 'rop_frag_dual' if name == 'dual' else 'rop_frag_r' if FORMATS[fmt][0] == 1 else 'rop_frag'
        lines.append(line(fmt, name, mask, f, fragment))
    for fmt, name, mask in UNREAD:
        f = [FACTOR[x] if x in FACTOR else OP[x] for x in BLENDS[name]]
        lines.append(line(fmt, f'{name}_unread', mask, f, 'rop_frag_unread'))
    for fmt, name, mask in CONSTANT:
        f = [FACTOR[x] if x in FACTOR else OP[x] for x in BLENDS[name]]
        lines.append(line(fmt, f'{name}_const', mask, f, 'rop_frag_const'))
    for fmt, mask in UNBLENDED:
        lines.append(line(fmt, 'none', mask, [1, 0, 0, 1, 0, 0], 'rop_frag', blending=0))
    return lines


def line(fmt: int, name: str, mask: int, f: list[int], fragment: str, blending: int = 1) -> str:  # noqa: PLR0913, PLR0917  a state's fields
    c0 = ':'.join(str(v) for v in [fmt, mask, blending, *f])
    return f'R ok=1 label=ropcheck.{fmt}_{name}_{mask} samples=1 topo=0 ds=0/0 c0={c0} | v rop_vert  | f {fragment}  | '


def neg(x: int) -> int:
    return x ^ 0x80000000


def unit(x: int) -> int:
    v = struct.unpack('<f', struct.pack('<I', flush(x, F32)))[0]
    if math.isnan(v) or v <= 0:
        return ZERO
    return ONE if v >= 1 else x


def combine(op: int, ts: int | None, td: int | None) -> int:
    """Give the blend of two terms (None: dropped) under operation `op` (add, subtract, reverse subtract)."""
    first, second = (td, ts) if op == 2 else (ts, td)
    if first is not None and second is not None:
        return arith('add', [first, second if op == 0 else neg(second)], F32, ftz=True)
    if first is not None:
        return first
    if second is not None:
        return second if op == 0 else neg(second)
    return ZERO


def exact(code: int, top: int) -> int:
    return encode(Fraction(code, top), F32)


class Model:
    """The blend of one state, from its key's c0 field."""

    def __init__(self, key: str):
        c0 = [int(v) for v in re.search(r'c0=(\S+)', key).group(1).split(':')]
        self.fmt, self.mask = c0[0], c0[1]
        self.factors = c0[3:]
        self.components, self.bits, self.half = FORMATS[self.fmt]
        self.vector = 'rop_frag_r' not in key
        self.constant_green = 'rop_frag_const' in key
        self.packed = PACKED.get(self.fmt)
        self.blending = bool(c0[2])

    def source(self, sampled: list[int]) -> list[int]:
        """Give the output the function computes of its sample (the blend's source)."""
        if self.constant_green:
            return [sampled[0], ZERO, sampled[2], sampled[3]]
        return sampled if self.vector else [sampled[0], ZERO, ZERO, ONE]

    def blend(self, s: list[int], s1: list[int], d: list[int], c: list[int]) -> list[int]:  # noqa: C901  one closure per factor rule
        if self.bits:
            s, s1, c = [unit(x) for x in s], [unit(x) for x in s1], [unit(x) for x in c]
        if self.components < 4:
            d = [*d[:3], ONE]

        def factor(f: int, k: int, alpha: bool) -> int | None:
            sub = lambda x: arith('add', [ONE, neg(x)], F32, ftz=True)  # noqa: E731
            table = {
                2: s[k],
                3: sub(s[k]),
                4: s[3],
                5: sub(s[3]),
                6: d[k],
                7: sub(d[k]),
                8: d[3],
                9: sub(d[3]),
                11: c[k],
                12: sub(c[k]),
                13: c[3],
                14: sub(c[3]),
                15: s1[k],
                16: sub(s1[k]),
                17: s1[3],
                18: sub(s1[3]),
            }
            if f == 10 and not alpha:
                return fminmax(s[3], sub(d[3]), True, True)
            return table.get(f)

        def term(x: int, f: int, k: int, alpha: bool) -> int | None:
            if not self.vector and f in (4, 5):
                f = 1 if f == 4 else 0
            if f == 0:
                return None
            if f == 1 or (f == 10 and alpha):
                return x
            return arith('mul', [x, factor(f, k, alpha)], F32, ftz=True)

        out = []
        for k in range(4):
            fs, fd, op = self.factors[3:6] if k == 3 else self.factors[:3]
            if op in (3, 4):
                out.append(fminmax(s[k], d[k], op == 3, True))
                continue
            out.append(combine(op, term(s[k], fs, k, k == 3), term(d[k], fd, k, k == 3)))
        return out

    def stored(self) -> list[int]:
        return [k for k in range(self.components) if self.mask >> (3 - k) & 1]

    def packed_word(self, want: list[int], loaded: int) -> int:
        """Give the texel a packed format's program stores: each stored channel's nearest code, the kept ones'."""
        out = 0
        for k, (at, bits) in enumerate(zip(self.packed, self.bits, strict=True)):
            top = (1 << bits) - 1
            stored = want[k] if self.blending else convert(convert(want[k], F32, F16), F16, F32)
            code = nearest_code(stored, top) if k in self.stored() else loaded >> at & top
            out |= code << at
        return out

    def kept(self) -> list[int]:
        """Give the format's channels the write mask keeps, which the draw's push word names."""
        return [k for k in range(self.components) if not self.mask >> (3 - k) & 1]


def word(rng: random.Random, normalized: bool) -> int:
    r = rng.random()
    if r < 0.2:
        return rng.choice(SPECIALS)
    if r < 0.3 and not normalized:
        return rng.getrandbits(32)
    return struct.unpack('<I', struct.pack('<f', rng.uniform(-0.25, 1.25)))[0]


def nearest_code(x: int, top: int) -> int:
    """Give the code of `x` (an f32's bits) in a channel of largest code `top`.

    The value is clamped to [0, 1] (NaN 0) and its exact product with `top` rounded to nearest even.
    """
    v = struct.unpack('<f', struct.pack('<I', x))[0]
    v = 0.0 if math.isnan(v) else min(max(v, 0.0), 1.0)
    return round(Fraction(v) * top)


def destination(rng: random.Random, m: Model) -> tuple[list[int], list[int]]:
    """Give a destination texel as the surface load gives it and its exact value.

    A normalized code loads as its exact value or as the product with the reciprocal of its top code.
    """
    if m.bits:
        codes = [rng.randrange(1 << b) for b in m.bits] + [0] * (4 - len(m.bits))
        tops = [(1 << b) - 1 for b in m.bits] + [1] * (4 - len(m.bits))
        exact_d = [exact(c, t) for c, t in zip(codes, tops, strict=True)]
        if m.packed:
            return [sum(c << at for c, at in zip(codes, m.packed, strict=True)), 0, 0, 0], exact_d
        if rng.random() < 0.5:
            return exact_d, exact_d
        loaded = [
            arith('mul', [encode(Fraction(c), F32), encode(Fraction(1, t), F32)], F32)
            for c, t in zip(codes, tops, strict=True)
        ]
        return loaded, exact_d
    halves = [rng.choice([rng.getrandbits(16), 0x7C00, 0xFC00, 0x7E00, 0x8000, 0x0001, 0x3C00]) for _ in range(4)]
    d = [convert(h, F16, F32) for h in halves] if m.half else [word(rng, False) for _ in range(4)]
    return d, d


def lane_inputs(  # noqa: PLR0913, PLR0917  one lane's every input
    sampled: list[int], s1: list[int], c: list[int], loaded: list[int], push: dict[int, int], kept: int, bound: int
) -> Inputs:
    def texture(op: tuple) -> list[int]:
        """Give a fetch's result by its canonical key (texture.py): the sample at x 0.75 is the second source's."""
        if op[0] == 'suld':
            return loaded
        return s1 if op[2] and op[2][0] == SECOND_SAMPLE else sampled

    def cbuf(bank: int, offset: int) -> int:
        if bank == 0 and offset - PUSH_BASE in push:
            return c[push[offset - PUSH_BASE]]
        if bank == 0 and PUSH_BASE <= offset < PUSH_BASE + 0x80:
            return kept
        return 0

    return Inputs(lambda a: 0x3F800000 if a == 0x7C else 0, cbuf, texture, bound)


def check(fs: bytes, key: str, push: dict[int, int], n: int, seed: int, bound: int = -1) -> str:  # noqa: PLR0913, PLR0917
    """`bound`: the constant buffer the program's bound textures read their handles from (-1: none)."""
    m = Model(key)
    code = fs[isa.HEADER_BYTES :]
    rng = random.Random(seed)
    for i in range(n):
        sampled = [word(rng, bool(m.bits)) for _ in range(4)]
        s1 = [word(rng, bool(m.bits)) for _ in range(4)]
        s = m.source(sampled)
        c = [word(rng, bool(m.bits)) for _ in range(4)]
        loaded, d = destination(rng, m)
        kept = sum(1 << k for k in m.kept())
        quad = [lane_inputs(sampled, s1, c, loaded, push, kept, bound) for _ in range(LANES)]
        want = m.blend(s, s1, d, c)
        for lane, (_, killed, stores) in enumerate(run_quad(code, quad, 0)):
            if killed or len(stores) != 1:
                return f'different: quad {i} lane {lane}: {len(stores)} stores, killed {killed}'
            got = list(stores[0][2])
            if m.packed:
                if got[:1] != [m.packed_word(want, loaded[0])]:
                    return f'different: quad {i} lane {lane}: {got[:1]} vs {m.packed_word(want, loaded[0]):#x}'
                continue
            for k in m.kept():
                if k >= len(got) or got[k] != loaded[k]:
                    seen = hex(got[k]) if k < len(got) else None
                    return f'different: quad {i} kept lane {k}: {seen} vs {hex(loaded[k])}'
            for k in m.stored():
                if k >= len(got) or got[k] != want[k]:
                    return (
                        f'different: quad {i} lane {k}: {hex(got[k]) if k < len(got) else None} vs {hex(want[k])} '
                        f'(s {[hex(x) for x in s]} d {[hex(x) for x in d]} c {[hex(x) for x in c]} '
                        f's1 {[hex(x) for x in s1]})'
                    )
    return 'identical'


def blend_color_words(record: dict) -> dict[int, int]:
    word_ = record.get('half_blend_color_word', 0)
    return {4 * (word_ - 1 + k): k for k in range(4)} if word_ else {}


def bound_cbuf(record: dict) -> int:
    """Give the constant buffer a dump's fragment program reads its bound textures' handles from, -1 for none."""
    return record.get('fs_bound_textures', 0) - 1


def records(path: Path) -> list[dict]:
    """Give the states of a dump directory, or of one JSON list of them (testdata/rop_blend/states.json)."""
    if path.is_dir():
        return [json.loads(f.read_text()) for f in sorted(path.glob('*.json'))]
    return json.loads(path.read_text())


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('dumps', type=Path, nargs='?')
    ap.add_argument('--corpus', action='store_true')
    ap.add_argument('--invocations', type=int, default=200)
    ap.add_argument('--seed', type=int, default=1)
    a = ap.parse_args()
    if a.corpus:
        print('\n'.join(corpus()))
        return
    failed = False
    for record in records(a.dumps):
        if 'rop_frag_unread' in record['key']:
            kept_device = (record.get('storage_mask'), record.get('storage_blend_mask')) == (0, 0)
            failed |= not kept_device
            print(f'{record["key"].split(" | ")[0]}: {"device blend" if kept_device else "storage form"}')
            continue
        verdict = check(
            bytes.fromhex(record['fs']),
            record['key'],
            blend_color_words(record),
            a.invocations,
            a.seed,
            bound_cbuf(record),
        )
        failed |= verdict != 'identical'
        print(f'{record["key"].split(" | ")[0]}: {verdict}')
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
