# ruff: noqa: N803, N806  S, D, K and A, B, C, Dd are the source, destination, constants and operands MODEL names
"""Evaluator for iterated-blend programs under the semantic model RUNG3.md states, and the check against reference.py.

The header gives encodings only; MODEL names what the evaluator assumes beyond them.
"""

import math
import random
import struct
import zlib
from collections.abc import Callable, Iterator, Sequence

from ucode import Program, half_value

MODEL = {
    'ops': 'MMADD A*B+C*D, MMSUB A*B-C*D, MIN min(A,B), MAX max(A,B), RCP 1/A, ADD B+D, SUB B-D',
    'pbr': 'PBR operand = the previous pass result, whatever PASS_OUTPUT names',
    'cc': 'SET_CC records each lane of the result against 0; TEST_CC predicates per lane; skipped lanes keep values',
    'swz': 'swizzle applies to the result before it is written (RRR/GGG/BBB broadcast, GBR rotates)',
    'rcp': 'RCP is a reciprocal accurate enough for the guard e = 1.0e-4',
    'k': 'constants are fp16 bit patterns, read as RGB vectors',
    'k_range': 'constants outside [0, 1] (negative or above 1) reach the datapath unclamped',
    'ff_alpha': 'ENABLE_RGB (0x1) takes alpha from SET_BLEND_ALPHA_*',
    'r_to_a': 'ENABLE_RGBA (0x3) takes alpha from the R lane of an R_TO_A pass',
    'out_t0': 'the colour written is T0 after the last pass',
}
NAN = float('nan')


def tags(prog: Program) -> frozenset[str]:
    t = {'ops', 'out_t0', 'r_to_a' if prog.alpha[0] == 'ucode' else 'ff_alpha'}
    for i in prog.code:
        if 'PBR' in (i.a, i.b, i.c, i.d):
            t.add('pbr')
        if i.test != 'T' or i.set_cc:
            t.add('cc')
        if i.swz in ('GBR', 'RRR', 'GGG', 'BBB'):
            t.add('swz')
        if i.op == 'RCP':
            t.add('rcp')
    if prog.consts:
        t.add('k')
    if any(not 0 <= half_value(b) <= 1 for v in prog.consts.values() for b in v):
        t.add('k_range')
    return frozenset(t)


def r16(x: float) -> float:
    try:
        return struct.unpack('<e', struct.pack('<e', x))[0]
    except OverflowError:
        return math.copysign(math.inf, x)


def fmin(a: float, b: float) -> float:
    return NAN if math.isnan(a) or math.isnan(b) else min(a, b)


def fmax(a: float, b: float) -> float:
    return NAN if math.isnan(a) or math.isnan(b) else max(a, b)


def rcp(x: float) -> float:
    if x == 0:
        return math.copysign(math.inf, x)
    return 1.0 / x


OPS: dict[str, Callable[[float, float, float, float], float]] = {
    'MMADD': lambda a, b, c, d: a * b + c * d,
    'MMSUB': lambda a, b, c, d: a * b - c * d,
    'MIN': lambda a, b, _c, _d: fmin(a, b),
    'MAX': lambda a, b, _c, _d: fmax(a, b),
    'RCP': lambda a, _b, _c, _d: rcp(a),
    'ADD': lambda _a, b, _c, d: b + d,
    'SUB': lambda _a, b, _c, d: b - d,
}


def passes(cc: float, test: str) -> bool:
    if test in ('T', 'F'):
        return test == 'T'
    if math.isnan(cc):
        return False
    return {
        'F': False,
        'T': True,
        'EQ': cc == 0,
        'NE': cc != 0,
        'LT': cc < 0,
        'LE': cc <= 0,
        'GT': cc > 0,
        'GE': cc >= 0,
    }[test]


def run(prog: Program, S: Sequence[float], D: Sequence[float], fp16: bool = False) -> tuple[float, ...]:
    rnd = r16 if fp16 else float
    S, D = [rnd(x) for x in S], [rnd(x) for x in D]
    reg = {'T0': [NAN] * 3, 'T1': [NAN] * 3, 'T2': [NAN] * 3, 'PBR': [NAN] * 3}
    cc, alpha = [NAN] * 3, NAN
    K = {j: [half_value(b) for b in v] for j, v in prog.consts.items()}

    def ac(x: str) -> list[float]:
        return {'SRC': S[:3], 'DST': D[:3], 'SRC_A': [S[3]] * 3, 'DST_A': [D[3]] * 3}.get(x) or reg[x]

    def bd(x: str, k: int) -> list[float]:
        fixed = {'ZERO': [0.0] * 3, 'ONE': [1.0] * 3, '1-SRC_A': [1 - S[3]] * 3, '1-DST_A': [1 - D[3]] * 3}
        if x in fixed:
            return fixed[x]
        return K[k] if x == 'K' else ac(x)

    for n, i in enumerate(prog.code):
        A, B, C, Dd = ac(i.a), bd(i.b, i.k), ac(i.c), bd(i.d, i.k)
        r = [OPS[i.op](A[lane], B[lane], C[lane], Dd[lane]) for lane in range(3)]
        r = [rnd(x) for x in r]
        if i.swz == 'R_TO_A':
            assert (i.test, n) == ('T', len(prog.code) - 2), 'R_TO_A must be the pass before the final move'
            alpha = r[0]
            reg[i.dest] = [NAN] * 3
            reg['PBR'] = [NAN] * 3
            continue
        r = {'RGB': r, 'GBR': [r[1], r[2], r[0]], 'RRR': [r[0]] * 3, 'GGG': [r[1]] * 3, 'BBB': [r[2]] * 3}[i.swz]
        lanes = {'RGB': (0, 1, 2), 'R': (0,), 'G': (1,), 'B': (2,)}[i.mask]
        live = [lane in lanes and passes(cc[lane], i.test) for lane in range(3)]
        for lane in range(3):
            reg[i.dest][lane] = r[lane] if live[lane] else reg[i.dest][lane]
            reg['PBR'][lane] = r[lane] if live[lane] else NAN
        if i.set_cc:
            assert i.test == 'T', 'SET_CC only on unpredicated passes here'
            cc = list(r)
    last = prog.code[-1]
    assert (last.dest, last.test, last.swz) == ('T0', 'T', 'RGB'), prog.name
    if prog.alpha[0] == 'ff':
        factor = {'ZERO': 0.0, 'ONE': 1.0, '1-SRC_A': 1 - S[3]}
        alpha = rnd(S[3] * factor[prog.alpha[1]] + D[3] * factor[prog.alpha[2]])
    return (*reg['T0'], alpha)


def store(c: tuple[float, ...], unorm: bool) -> tuple[float, ...]:
    if not unorm:
        return c
    return tuple(NAN if math.isnan(x) else min(1.0, max(0.0, x)) for x in c)


def inputs(seed: int, count: int, unorm_dst: bool = True) -> Iterator[tuple[list[float], list[float]]]:
    """Premultiplied pairs: edge alphas and colours, then random ones; D on the 8-bit grid when unorm_dst."""
    rng = random.Random(seed)
    alphas = (0.0, 1 / 255, 0.25, 0.5, 0.75, 1.0)
    fracs = (0.0, 0.3, 0.5, 0.7, 1.0)

    def colour(a: float, grid: bool) -> list[float]:
        c = [a * rng.choice(fracs) if rng.random() < 0.5 else a * rng.random() for _ in range(3)] + [a]
        return [round(x * 255) / 255 for x in c] if grid else c

    for _ in range(count):
        sa = rng.choice(alphas) if rng.random() < 0.5 else rng.random()
        da = rng.choice(alphas) if rng.random() < 0.5 else rng.random()
        yield colour(sa, False), colour(da, unorm_dst)


def check(
    prog: Program,
    ref: Callable[[list[float], list[float]], tuple[float, ...]],
    unorm: bool = True,
    count: int = 4000,
    tol: float = 1e-6,
) -> tuple[bool, float, float, tuple[list[float], list[float], tuple[float, ...], tuple[float, ...]] | None]:
    """Largest |program - reference| over the inputs, exact and with every pass rounded to fp16 (in 1/255 units)."""
    exact = fp16 = 0.0
    worst = None
    for S, D in inputs(zlib.crc32(prog.name.encode()), count, unorm):
        want = store(ref(S, D), unorm)
        got = store(run(prog, S, D), unorm)
        err = max(math.inf if math.isnan(a) or math.isnan(b) else abs(a - b) for a, b in zip(got, want, strict=True))
        if err > exact:
            exact, worst = err, (S, D, got, want)
        got16 = store(run(prog, S, D, fp16=True), unorm)
        err16 = max(
            math.inf if math.isnan(a) or math.isnan(b) else abs(a - b) for a, b in zip(got16, want, strict=True)
        )
        fp16 = max(fp16, err16)
    return exact <= tol, exact, fp16 * 255, worst
