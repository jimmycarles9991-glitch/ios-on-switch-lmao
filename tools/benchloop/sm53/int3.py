"""XMAD, LOP3.LUT, IADD3 and ISCADD: latency, issue rate, and NAK's XMAD multiply sequences against exact products.

stall       latency.stall's dependent chains, one warp, delays 1..15: the smallest right delay is the latency.
stall-wide  the same chains over the whole target at delays 5..15, beside FFMA (fixed) and MUFU.RCP (variable, no
            barrier): an op that is right at 6 under every warp's contention is fixed-latency, not merely fast alone.
throughput  latency.throughput's independent streams over the whole target, k = 512, 1024, 2048.
xmad-mul    per column a random (or edge) pair x, y: NAK's 3-XMAD x * y (sm50/xmad.rs imul), its 4-XMAD 64-bit high
            word and low word (umul_2x32_64) and the signed high word, each instruction 30 cycles after the last.
"""

import random
import struct

from tools.benchloop import columns, protocol
from tools.benchloop.sm53 import encode as e, latency, operands, ops, program, sph
from tools.benchloop.sm53.encode import RZ, Imm
from tools.benchloop.sm53.intops import LUT_XOR3, iadd3, iscadd, lop3, xmad
from tools.benchloop.sm53.ops import K1, K2, Recipe

RECIPES = (
    Recipe('XMAD', 'Xmad', lambda d, s: xmad(d, s, K1, s), 0x12345678, (3, 0), 0x5A5AA5A5, 0x00010003),
    Recipe('XMAD.IMM', 'Xmad', lambda d, s: xmad(d, s, Imm(3), s), 0x12345678, poison=0x5A5AA5A5, step=0x00010003),
    Recipe('XMAD.PSL', 'Xmad', lambda d, s: xmad(d, s, K1, s, 'psl'), 0x12345678, (3, 0), 0x5A5AA5A5, 0x00030001),
    Recipe('XMAD.MRG', 'Xmad', lambda d, s: xmad(d, K1, s, s, 'mrg'), 0x00001234, (2, 0), 0x0000A5A5, 0x00000003),
    Recipe(
        'XMAD.PSL.CBCC',
        'Xmad',
        lambda d, s: xmad(d, s, K1, s, 'a_hi b_hi psl cbcc'),
        0x12345678,
        (0x00020001, 0),
        0x5A5AA5A5,
        0x00030003,
    ),
    Recipe('LOP3', 'Lop3', lambda d, s: lop3(d, s, K1, K2, LUT_XOR3), 0x12345678, (0x5A5A5A5A, 0x0F0F0F0F), 0xC0000000),
    Recipe(
        'LOP3.IMM', 'Lop3', lambda d, s: lop3(d, s, Imm(0x1234), K2, LUT_XOR3), 0x12345678, (0, 0x0F0F0F0F), 3 << 30
    ),
    Recipe('IADD3', 'IAdd3', lambda d, s: iadd3(d, s, K1, K2), 0, (0x01010101, 0x10000001), 0x40000000),
    Recipe('ISCADD', 'Lea', lambda d, s: iscadd(d, s, s, 1), 0x12345679, poison=0x0000A5A5, step=2),
    Recipe('ISCADD.IMM', 'Lea', lambda d, s: iscadd(d, s, Imm(0x1235), 1), 0x12345678, poison=0x0000A5A4, step=2),
)
WIDE = (RECIPES[0], RECIPES[4], ops.RECIPES['FFMA'], ops.RECIPES['MUFU.RCP'])
WIDE_DELAYS = (5, 6, 7, 8, 10, 15)
MUL_COLUMNS = protocol.MAX_SIDE
MUL_RANDOM = 3 * MUL_COLUMNS
MUL_EDGES = (0, 1, 0xFFFF, 0x10000, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x80808081, 0x00FFFFFF)
SPACING = 15
X, Y = 8, 9


def stall_wide() -> list[dict]:
    out = []
    for r in WIDE:
        for d in WIDE_DELAYS:
            meta = {'family': 'stall-wide', 'op': r.key, 'delay': d}
            chain = latency._stall_gprs(r, d).fragment()
            x = latency.experiment(f'stallw.{r.slug}.d{d:02d}', chain, latency.WHOLE, meta, r.exact)
            out.append(x | {'draws': [1], 'repeats': 1, 'warmup': 0})
    return out


def _spaced(instrs: list[e.Instr]) -> list[e.Instr]:
    """Give `instrs` each 2 x SPACING cycles after the last, far past any fixed latency the bench has seen."""
    return [x for i in instrs for x in (i.sched(delay=SPACING), e.nop().sched(delay=SPACING))]


def _products() -> list[e.Instr]:
    """Give NAK's sequences on x = R8, y = R9: R12 = x * y (imul), R21:R22 the 64-bit product, R28 the signed high."""
    return [
        xmad(10, Y, X, RZ),
        xmad(11, Y, X, RZ, 'b_hi mrg'),
        xmad(12, Y, 11, 10, 'a_hi b_hi psl cbcc'),
        xmad(13, X, Y, RZ),
        e.shr(14, 13, Imm(16)),
        xmad(15, X, Y, 14, 'a_hi'),
        e.lop32i(16, 15, 0xFFFF, 'and'),
        xmad(17, X, Y, 16, 'b_hi'),
        e.shr(18, 15, Imm(16)),
        xmad(19, X, Y, 18, 'a_hi b_hi'),
        e.shr(20, 17, Imm(16)),
        e.iadd(21, 19, 20),
        e.prmt(22, 13, Imm(0x5410), 17),
        e.shr(23, X, Imm(31), 's32'),
        e.lop(24, 23, Y, 'and'),
        e.iadd(25, 21, 24, 'neg_b'),
        e.shr(26, Y, Imm(31), 's32'),
        e.lop(27, 26, X, 'and'),
        e.iadd(28, 25, 27, 'neg_b'),
    ]


# program: the two registers its texel carries, the host's value of each
MUL_PROGRAMS = {
    'mul32-hi': ((12, 21), lambda x, y: (x * y & 0xFFFFFFFF, x * y >> 32)),
    'lo-shi': ((22, 28), lambda x, y: (x * y & 0xFFFFFFFF, _signed(x) * _signed(y) >> 32 & 0xFFFFFFFF)),
}


def _signed(v: int) -> int:
    return v - (1 << 32) if v >> 31 else v


def mul_code(results: tuple[int, int]) -> bytes:
    """Give the column program: load column x's pair from c[1][16x] into R8, R9, run _products, write `results`."""
    front = e.unbundle(columns.code())
    front = front[: [i.name for i in front].index('EXIT')]
    load = [columns.ldc64(X, 4, program.DATA_BANK, 0).sched(wr_bar=0, delay=e.BARRIER_SET_CYCLES)]
    body = front[:3] + load + [program.SEPARATOR] + _spaced(_products())
    return e.bundle(e.finish([*body, program.SEPARATOR, *program.epilogue(results)]))


def mul_pairs() -> list[tuple[int, int]]:
    rng = random.Random(53)
    pairs = [(x, y) for x in MUL_EDGES for y in MUL_EDGES]
    return pairs + [(rng.getrandbits(32), rng.getrandbits(32)) for _ in range(MUL_RANDOM - len(pairs))]


def mul_experiments() -> list[dict]:
    out = []
    pairs = mul_pairs()
    for name, (regs, _) in MUL_PROGRAMS.items():
        code = mul_code(regs)
        for part in range(0, len(pairs), MUL_COLUMNS):
            chunk = pairs[part : part + MUL_COLUMNS]
            data = b''.join(struct.pack('<4I', x, y, 0, 0) for x, y in chunk)
            p = {
                'sph': protocol.b64(sph.pixel(operands.attributes(code))),
                'code': protocol.b64(code),
                'data': protocol.b64(data),
                'gprs': operands.highest_register(code) + 1,
            }
            x = {
                'id': f'xmadmul.{name}.{part // MUL_COLUMNS}',
                'kind': 'readback',
                'target': {'format': 'rgba32float', 'width': len(chunk), 'height': 1},
                'program': p,
                'readback': 'full',
                'meta': {'family': 'xmad-mul', 'program': name},
            }
            protocol.validate_experiment(x)
            out.append(x)
    return out


def generate() -> list[dict]:
    return [
        latency.warm_up('int3.warm'),
        *latency.ladder(),
        *latency.stall(RECIPES),
        *stall_wide(),
        *latency.throughput(RECIPES),
        *mul_experiments(),
    ]


def _wide_op(group: list[dict], results: dict[str, dict]) -> dict:
    """Give the op's delays as '+' (every pixel the reference texel), '-' (else) and '?' (no result)."""
    texel = {}
    for x in group:
        r = results.get(x['id']) or {}
        distinct = (r.get('texels') or {}).get('distinct') or []
        uniform = r.get('status') == 'ok' and len(distinct) == 1
        texel[x['meta']['delay']] = distinct[0]['value'] if uniform else None
    want = program.texel_hex(group[0]['expect']['texel'])
    if not group[0]['expect']['exact']:
        want = texel.get(max(WIDE_DELAYS))
    pattern = ''.join('?' if d not in texel else '+' if want and texel[d] == want else '-' for d in WIDE_DELAYS)
    return {'delays': list(WIDE_DELAYS), 'pattern': pattern, 'right_from': _right_from(pattern)}


def _right_from(pattern: str) -> int | None:
    first = None
    for d, c in reversed(list(zip(WIDE_DELAYS, pattern, strict=True))):
        if c != '+':
            break
        first = d
    return first


def analyse_wide(experiments: list[dict], results: dict[str, dict]) -> dict:
    groups: dict[str, list[dict]] = {}
    for x in experiments:
        if x['meta'].get('family') == 'stall-wide':
            groups.setdefault(x['meta']['op'], []).append(x)
    return {op: _wide_op(g, results) for op, g in groups.items()}


def pairs_of(x: dict) -> list[tuple[int, int]]:
    data = protocol.unb64(x['program']['data'])
    return [struct.unpack_from('<2I', data, k) for k in range(0, len(data), 16)]


def analyse_mul(experiments: list[dict], results: dict[str, dict]) -> dict:
    """Give, per program, how many pairs came back and how many of each word differ from the exact product."""
    out: dict[str, dict] = {}
    for x in experiments:
        if x['meta'].get('family') != 'xmad-mul':
            continue
        name = x['meta']['program']
        row = out.setdefault(name, {'checked': 0, 'wrong': [0, 0], 'first_wrong': None, 'missing': 0})
        r = results.get(x['id']) or {}
        data = protocol.unb64((r.get('texels') or {}).get('data') or '')
        if r.get('status') != 'ok' or not data:
            row['missing'] += 1
            continue
        for k, (a, b) in enumerate(pairs_of(x)):
            got = program.unpack(list(struct.unpack_from('<4I', data, 16 * k)))
            want = MUL_PROGRAMS[name][1](a, b)
            row['checked'] += 1
            for w in (0, 1):
                if got[w] != want[w]:
                    row['wrong'][w] += 1
                    row['first_wrong'] = row['first_wrong'] or {'x': hex(a), 'y': hex(b), 'got': got, 'want': want}
    return out
