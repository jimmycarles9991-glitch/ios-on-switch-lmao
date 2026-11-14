"""HSETP2: its predicate latency and issue rate, and what it computes against the interpreter's hsetp2().

stall       latency.stall's predicate chains (p = (K1 < K2) xor accum, one warp, delays 1..15) for HSETP2 beside FSETP
            and ISETP: NAK gives HSETP2's predicate 14 cycles, FSETP's measured 13.
throughput  latency.throughput's predicate streams for the three, k = 512, 1024, 2048.
hsetp2      per column a pair of half2 operands (edge halves against each other, then random ones); each program runs
            up to 32 HSETP2 forms (every comparison, .FTZ, .H_AND, the half swizzles, |a| -a |b| -b, the accumulator
            and combining op) and writes each form's two predicates out as bits through SEL. Every HSETP2 issues 30
            cycles before the next, so its result is in whatever its latency.
"""

import random
import struct

from tools.benchloop import columns, protocol
from tools.benchloop.sm53 import encode as e, latency, operands, ops, program, sph
from tools.benchloop.sm53.encode import FLOAT_CMP, PT, RZ, Imm, Pred, half2
from tools.benchloop.sm53.halfops import hsetp2
from tools.benchloop.sm53.ops import K1, K2, Recipe
from tools.gpu.maxwell.execute import alu
from tools.gpu.maxwell.interp import Lane

RECIPE = Recipe(
    'HSETP2',
    'HSetP2',
    lambda p, q: hsetp2((p, PT), K1, K2, 'lt xor', q),
    0,
    (half2(1.0, 1.0), half2(2.0, 2.0)),
    pred=True,
)
TIMED = (RECIPE, ops.RECIPES['FSETP'], ops.RECIPES['ISETP'])
A, B, ONE, BIT0, BIT1, ACC_X, ACC_Y = 8, 9, 10, 11, 12, 13, 14
TRUE, FALSE = 4, 5
ACCUMS = {'pt': PT, 'p4': TRUE, 'p5': FALSE, '!p4': Pred(TRUE, neg=True), '!p5': Pred(FALSE, neg=True)}
PER_PROGRAM = 32
COLUMNS = protocol.MAX_SIDE
EDGES = (0x0000, 0x8000, 0x7C00, 0xFC00, 0x7E00, 0xFE00, 0x7C01, 0x7FFF, 0x0001, 0x8001, 0x03FF, 0x83FF)
EDGES += (0x0400, 0x8400, 0x3C00, 0xBC00, 0x4000, 0x3800, 0x7BFF, 0xFBFF, 0x3C01, 0x3BFF)
HALF_SWIZZLES = ('h1h0', 'h0h0', 'h1h1')
MOD_SETS = ('abs_a', 'neg_a', 'abs_b', 'neg_b', 'abs_a neg_a', 'abs_b neg_b')


def variants() -> list[tuple[str, str]]:
    """Give every (test, accumulator) form the readback runs."""
    out = [(f'{c} and', 'pt') for c in FLOAT_CMP]
    out += [(f'{c} and ftz', 'pt') for c in FLOAT_CMP]
    out += [(f'{c} and h_and', 'pt') for c in FLOAT_CMP]
    out += [(f'lt and a={sa} b={sb}', 'pt') for sa in HALF_SWIZZLES for sb in HALF_SWIZZLES]
    out += [(f'{c} and {m}', 'pt') for c in ('lt', 'eq', 'gtu', 'ne') for m in MOD_SETS]
    out += [(f'lt {op}', acc) for op in ('and', 'or', 'xor') for acc in ('p4', 'p5', '!p4', '!p5')]
    out += [(f'ge {op} h_and', acc) for op in ('and', 'or', 'xor') for acc in ('p4', '!p5')]
    return out


def form(test: str, accum: str) -> e.Instr:
    return hsetp2((0, 1), A, B, test, ACCUMS[accum])


def _bits(acc: int) -> list[e.Instr]:
    """Give acc = (acc << 2) | P0 << 1 | P1, from the predicates the k-th form just wrote."""
    return [
        e.sel(BIT0, ONE, RZ, 0).sched(delay=program.FIXED_LATENCY),
        e.sel(BIT1, ONE, RZ, 1).sched(delay=program.FIXED_LATENCY),
        e.shl(acc, acc, Imm(1)).sched(delay=program.FIXED_LATENCY),
        e.iadd(acc, acc, BIT0).sched(delay=program.FIXED_LATENCY),
        e.shl(acc, acc, Imm(1)).sched(delay=program.FIXED_LATENCY),
        e.iadd(acc, acc, BIT1).sched(delay=program.FIXED_LATENCY),
    ]


def code(forms: list[tuple[str, str]]) -> bytes:
    """Give the column program: column x's operands from c[1][16x] into A, B, each form's bits into ACC_X, ACC_Y."""
    front = e.unbundle(columns.code())
    front = front[: [i.name for i in front].index('EXIT')]
    head = [
        *front[:3],
        columns.ldc64(A, 4, program.DATA_BANK, 0).sched(wr_bar=0, delay=e.BARRIER_SET_CYCLES),
        e.isetp(TRUE, RZ, RZ, 'eq').sched(delay=1),
        e.isetp(FALSE, RZ, RZ, 'ne').sched(delay=1),
        e.mov32i(ONE, 1).sched(delay=1),
        e.mov32i(ACC_X, 0).sched(delay=1),
        e.mov32i(ACC_Y, 0).sched(delay=1),
        program.SEPARATOR,
    ]
    body = []
    for k, (test, accum) in enumerate(forms):
        spacing = e.nop().sched(delay=program.SAFE_DELAY)
        body += [form(test, accum).sched(delay=program.SAFE_DELAY), spacing, *_bits(ACC_X if k < 16 else ACC_Y)]
    return e.bundle(e.finish([*head, *body, program.SEPARATOR, *program.epilogue((ACC_X, ACC_Y))]))


def operand_pairs() -> list[tuple[int, int]]:
    """Give each column's (a, b) half2 words: every edge half against every other in the low halves, then random."""
    n = len(EDGES)
    out = [
        (EDGES[i] | EDGES[(i + j) % n] << 16, EDGES[j] | EDGES[(3 * i + 7 * j + 1) % n] << 16)
        for i in range(n)
        for j in range(n)
    ]
    rng = random.Random(2)
    return out + [(rng.getrandbits(32), rng.getrandbits(32)) for _ in range(COLUMNS - len(out))]


def expected(forms: list[tuple[str, str]], a: int, b: int) -> tuple[int, int]:
    """Give the two words the program writes for operands a, b: each form's P, Q from the interpreter's hsetp2()."""
    words = [0, 0]
    for k, (test, accum) in enumerate(forms):
        lane = Lane()
        lane.w(A, a)
        lane.w(B, b)
        lane.setp(TRUE, True)
        lane.setp(FALSE, False)
        i = form(test, accum)
        alu(i.name, i.word, lane, program.inputs(b''))
        w = k // 16
        words[w] = (words[w] << 2 | int(lane.p(0, 0)) << 1 | int(lane.p(1, 0))) & 0xFFFFFFFF
    return words[0], words[1]


def readbacks() -> list[dict]:
    pairs = operand_pairs()
    data = protocol.b64(b''.join(struct.pack('<4I', a, b, 0, 0) for a, b in pairs))
    every = variants()
    out = []
    for g in range(0, len(every), PER_PROGRAM):
        forms = every[g : g + PER_PROGRAM]
        c = code(forms)
        x = {
            'id': f'hsetp2.forms.{g // PER_PROGRAM}',
            'kind': 'readback',
            'target': {'format': 'rgba32float', 'width': len(pairs), 'height': 1},
            'program': {
                'sph': protocol.b64(sph.pixel(operands.attributes(c))),
                'code': protocol.b64(c),
                'data': data,
                'gprs': ACC_Y + 1,
            },
            'readback': 'full',
            'meta': {'family': 'hsetp2', 'forms': [list(f) for f in forms]},
        }
        protocol.validate_experiment(x)
        out.append(x)
    return out


def generate() -> list[dict]:
    return [latency.warm_up('hsetp2.warm'), *latency.stall(TIMED), *latency.throughput(TIMED), *readbacks()]


def pairs_of(x: dict) -> list[tuple[int, int]]:
    data = protocol.unb64(x['program']['data'])
    return [struct.unpack_from('<2I', data, k) for k in range(0, len(data), 16)]


def analyse(experiments: list[dict], results: dict[str, dict]) -> dict:
    """Give per form the columns whose P or Q differs from the interpreter's, and the first such operands."""
    out: dict = {}
    for x in experiments:
        if x['meta'].get('family') != 'hsetp2':
            continue
        forms = [tuple(f) for f in x['meta']['forms']]
        r = results.get(x['id']) or {}
        data = protocol.unb64((r.get('texels') or {}).get('data') or '')
        if r.get('status') != 'ok' or not data:
            out[x['id']] = {'status': r.get('status', 'missing'), 'why': r.get('why', '')}
            continue
        rows = {f'{t} [{acc}]': {'p_off': 0, 'q_off': 0, 'first': None} for t, acc in forms}
        counts = (min(16, len(forms)), len(forms) - 16)
        for col, (a, b) in enumerate(pairs_of(x)):
            got = program.unpack(list(struct.unpack_from('<4I', data, 16 * col)))
            want = expected(forms, a, b)
            for k, (t, acc) in enumerate(forms):
                shift = 2 * (counts[k // 16] - 1 - k % 16)
                g, w = got[k // 16] >> shift & 3, want[k // 16] >> shift & 3
                if g != w:
                    row = rows[f'{t} [{acc}]']
                    row['p_off'] += (g ^ w) >> 1
                    row['q_off'] += (g ^ w) & 1
                    row['first'] = row['first'] or {'a': f'{a:#010x}', 'b': f'{b:#010x}', 'got': g, 'want': w}
        out[x['id']] = {'status': 'ok', 'columns': len(data) // 16, 'forms': rows}
    return out
