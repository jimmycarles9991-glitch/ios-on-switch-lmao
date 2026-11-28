"""The latency and control-bit questions NVIDIA's own GM20B code raises about NAK's SM50 tables.

reader       LINKS independent writes, each read `delay` cycles later (1..15, one warp); the reads' sum is the texel, so
             one stale read changes it. The smallest right delay is how soon the reader collects its operand. NAK waits
             a fixed-latency write's 6 cycles whatever reads it (sm50.rs raw_latency); NVIDIA issues F2F, MUFU, F2I,
             I2F and FLO 2 cycles after the write and LDC 4. FFMA reading FFMA is the control: right from 6, no sooner.
             RRO read by MUFU.EX2 with no barrier on RRO asks whether RRO is fixed-latency (NAK scoreboards it, NVIDIA
             reads it 2 cycles later).
reader-wide  the same at a few delays over the whole target: right under every warp's contention, not merely alone.
stall HSETP2 latency.stall's predicate chain of HSETP2: NAK gives its predicate 14 cycles, every other compare 13.
yield        latency.reuse with bit 4 set and clear (dual issue is dualclock.py's, timed inside the warp).
"""

from collections.abc import Callable
from dataclasses import dataclass

from tools.benchloop.sm53 import encode as e, latency, program
from tools.benchloop.sm53.encode import PT, f32, half2
from tools.benchloop.sm53.halfops import hsetp2
from tools.benchloop.sm53.intops import xmad
from tools.benchloop.sm53.ops import K1, K2, Recipe
from tools.benchloop.sm53.program import SEPARATOR

LINKS = 6
DELAYS = range(1, 16)
WIDE_DELAYS = (1, 2, 3, 4, 6, 15)
# link j reads A0 + 4j and writes W0 + 4j, which its reader reads into V0 + 4j: three banks, no conflicts
A0, W0, V0 = 16, 17, 18
SUM, LAST = 40, V0 + 4 * (LINKS - 1)
READER_DELAY = e.BARRIER_SET_CYCLES
SUM_DELAY = 6


@dataclass(frozen=True)
class Pair:
    """A writer, the instruction that reads its result, and the values the links start from."""

    key: str
    write: Callable[[int, int], e.Instr]
    read: Callable[[int, int], e.Instr]
    operands: Callable[[dict[int, int]], list[int]]
    poison: Callable[[dict[int, int]], int]
    consts: tuple[int, int] = (0, 0)
    integer: bool = False
    exact: bool = True
    scoreboarded: bool = True

    @property
    def slug(self) -> str:
        return self.key.lower().replace('>', '-').replace('.', '-')


def _rank(init: dict[int, int], reg: int) -> int:
    return sorted(init).index(reg)


def _floats(_: dict[int, int]) -> list[int]:
    return [f32(2.0 * j + 0.25) for j in range(LINKS)]


def _ints(_: dict[int, int]) -> list[int]:
    return [0x1000 * j + 0x35 for j in range(LINKS)]


def _ldc_offsets(init: dict[int, int]) -> list[int]:
    """Link j's address is the byte offset of link j + 1's operand in c[1]: each load finds a different word."""
    return [4 * _rank(init, A0 + 4 * ((j + 1) % LINKS)) for j in range(LINKS)]


def _ffma(d: int, a: int) -> e.Instr:
    return e.ffma(d, a, K1, K2)


def _iadd(d: int, a: int) -> e.Instr:
    return e.iadd(d, a, K1)


PAIRS_READ = (
    Pair('FFMA>FFMA', _ffma, _ffma, _floats, lambda _: f32(1000.25), (f32(2.0), f32(1.0)), scoreboarded=False),
    Pair('FFMA>F2F', _ffma, lambda d, s: e.f2f(d, s, 'round'), _floats, lambda _: f32(1000.25), (f32(2.0), f32(1.0))),
    Pair(
        'FFMA>MUFU.RCP',
        _ffma,
        lambda d, s: e.mufu(d, s, 'rcp'),
        _floats,
        lambda _: f32(1000.25),
        (f32(2.0), f32(1.0)),
        exact=False,
    ),
    Pair(
        'FFMA>F2I',
        _ffma,
        lambda d, s: e.f2i(d, s, 'rz'),
        _floats,
        lambda _: f32(1000.25),
        (f32(2.0), f32(1.0)),
        integer=True,
    ),
    Pair('IADD>I2F', _iadd, e.i2f, _ints, lambda _: 0x7000_0000, (0x0101_0000, 0)),
    Pair('IADD>FLO', _iadd, e.flo, lambda _: [1 << (j + 3) for j in range(LINKS)], lambda _: 1 << 30, integer=True),
    Pair(
        'IADD>LDC',
        _iadd,
        lambda d, s: e.ldc(d, s, 1),
        _ldc_offsets,
        lambda init: 4 * _rank(init, K2),
        (0, 0x7777_7777),
        integer=True,
    ),
    Pair(
        'RRO>MUFU.EX2',
        lambda d, a: e.rro(d, a, 'ex2'),
        lambda d, s: e.mufu(d, s, 'ex2'),
        lambda _: [f32(0.5 * j + 0.25) for j in range(LINKS)],
        lambda _: f32(100.0),
        exact=False,
    ),
)


def _init(p: Pair) -> dict[int, int]:
    init = {K1: p.consts[0], K2: p.consts[1]}
    init |= {A0 + 4 * j: 0 for j in range(LINKS)} | {W0 + 4 * j: 0 for j in range(LINKS)}
    values = p.operands(init)
    poison = p.poison(init)
    return init | {A0 + 4 * j: v for j, v in enumerate(values)} | {W0 + 4 * j: poison for j in range(LINKS)}


def reads(p: Pair, delay: int) -> tuple[list[e.Instr], dict[int, int]]:
    """Give the links at `delay` and the sum of their reads into SUM (behind a wait on every barrier)."""
    body = []
    for j in range(LINKS):
        a, w, v = A0 + 4 * j, W0 + 4 * j, V0 + 4 * j
        read = p.read(v, w).sched(delay=READER_DELAY)
        body += [p.write(w, a).sched(delay=delay), read.sched(wr_bar=0) if p.scoreboarded else read]
    add = e.iadd if p.integer else e.fadd
    body += [SEPARATOR, add(SUM, V0, V0 + 4).sched(delay=SUM_DELAY)]
    body += [add(SUM, SUM, V0 + 4 * j).sched(delay=SUM_DELAY) for j in range(2, LINKS)]
    return body, _init(p)


def _experiment(p: Pair, delay: int, family: str, rect: list[int]) -> dict:
    body, init = reads(p, delay)
    prog = program.fragment(body, init, (SUM, LAST))
    meta = {'family': family, 'op': p.key, 'delay': delay}
    tag = 'readw' if family == 'reader-wide' else 'read'
    x = latency.experiment(f'{tag}.{p.slug}.d{delay:02d}', prog, rect, meta, p.exact)
    return x | {'draws': [1], 'repeats': 1, 'warmup': 0}


def _xmad(d: int, a: int) -> e.Instr:
    return xmad(d, a, K1, K2)


def _halves(_: dict[int, int]) -> list[int]:
    return [half2(j + 0.25, j + 0.5) for j in range(LINKS)]


def _widen(d: int, s: int) -> e.Instr:
    return e.f2f(d, s, 'src=f16 dst=f32')


# The writers NAK's reader rule must hold for before it covers them: XMAD (kept at 6 until these pass) and the half ALU,
# whose results B, C and E feed to F2F on every frame. XMAD's product is a 16-bit operand times K1's low half, plus K2.
PAIRS_WRITERS = (
    Pair(
        'XMAD>LDC', _xmad, lambda d, s: e.ldc(d, s, 1), _ldc_offsets, lambda i: 4 * _rank(i, K2), (1, 0), integer=True
    ),
    Pair('XMAD>I2F', _xmad, e.i2f, _ints, lambda _: 0x7000, (1, 0)),
    Pair(
        'XMAD>FLO',
        _xmad,
        e.flo,
        lambda _: [1 << (j + 3) for j in range(LINKS)],
        lambda _: 1 << 14,
        (1, 0),
        integer=True,
    ),
    Pair(
        'XMAD>F2F',
        _xmad,
        lambda d, s: e.f2f(d, s, 'round'),
        lambda _: [0x100 * (j + 1) for j in range(LINKS)],
        lambda _: f32(1000.25),
        (1, f32(1.0)),
    ),
    Pair(
        'XMAD>MUFU.RCP',
        _xmad,
        lambda d, s: e.mufu(d, s, 'rcp'),
        lambda _: [0x100 * (j + 1) for j in range(LINKS)],
        lambda _: f32(1000.25),
        (1, f32(1.0)),
        exact=False,
    ),
    Pair(
        'HFMA2>F2F',
        lambda d, a: e.hfma2(d, a, K1, K2),
        _widen,
        _halves,
        lambda _: half2(-1000.0, -1000.0),
        (half2(2.0, 2.0), half2(1.0, 1.0)),
    ),
    Pair(
        'HADD2>F2F',
        lambda d, a: e.hadd2(d, a, K1),
        _widen,
        _halves,
        lambda _: half2(-1000.0, -1000.0),
        (half2(1.0, 1.0), 0),
    ),
)


def reader(pairs: tuple[Pair, ...] = PAIRS_READ) -> list[dict]:
    return [_experiment(p, d, 'reader', latency.ONE_WARP) for p in pairs for d in DELAYS]


def reader_wide(pairs: tuple[Pair, ...] = PAIRS_READ) -> list[dict]:
    return [_experiment(p, d, 'reader-wide', latency.WHOLE) for p in pairs for d in WIDE_DELAYS]


HSETP2 = Recipe(
    'HSETP2',
    'HSetP2',
    lambda p, q: hsetp2((p, PT), K1, K2, 'lt xor', q),
    0,
    (half2(1.0, 1.0), half2(2.0, 2.0)),
    pred=True,
)


def generate() -> list[dict]:
    return [latency.warm_up('readers.warm'), *latency.ladder(), *reader(), *reader_wide(), *latency.stall((HSETP2,))]


def generate_writers() -> list[dict]:
    """Give the reader rows for XMAD and half writers, one warp and wide."""
    return [
        latency.warm_up('writers.warm'),
        *latency.ladder(),
        *reader(PAIRS_WRITERS),
        *reader_wide(PAIRS_WRITERS),
    ]


def generate_yield() -> list[dict]:
    """Give reuse with bit 4 clear (every variant: 11 Nov ran three) and set."""
    return [latency.warm_up('yield.warm'), *latency.ladder(), *latency.reuse(), *latency.reuse(yld=True)]


def pattern(group: list[dict], results: dict[str, dict], delays: tuple[int, ...]) -> dict:
    """Give the delays as marks and the first delay from which every later one is right.

    A mark is '+' (every pixel the reference texel), '-' (any other) or '?' (no result). An inexact pair's reference is
    its own texel at the longest delay.
    """
    texel: dict[int, str | None] = {}
    for x in group:
        r = results.get(x['id']) or {}
        distinct = (r.get('texels') or {}).get('distinct') or []
        if r.get('status') == 'ok' and distinct:
            texel[x['meta']['delay']] = distinct[0]['value'] if len(distinct) == 1 else None
    want = program.texel_hex(group[0]['expect']['texel'])
    if not group[0]['expect']['exact']:
        want = texel.get(max(delays))
    marks = ''.join('?' if d not in texel else '+' if want and texel[d] == want else '-' for d in delays)
    right_from = None
    for d, c in reversed(list(zip(delays, marks, strict=True))):
        if c != '+':
            break
        right_from = d
    return {'delays': list(delays), 'pattern': marks, 'right_from': right_from}


def analyse(experiments: list[dict], results: dict[str, dict]) -> dict:
    out: dict = {}
    for family, delays in (('reader', tuple(DELAYS)), ('reader-wide', WIDE_DELAYS)):
        groups: dict[str, list[dict]] = {}
        for x in experiments:
            if x['meta'].get('family') == family:
                groups.setdefault(x['meta']['op'], []).append(x)
        out[family] = {op: pattern(g, results, delays) for op, g in groups.items()}
    return out
