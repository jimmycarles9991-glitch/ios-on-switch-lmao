"""fast::powr's native composition at the special operands: the raw bits of MUFU.LG2, FMUL.FTZ, RRO.EX2 and MUFU.EX2.

The translator wraps NAK's exp2(y * log2(x)) in Metal's special-value rules (math.rs fast_powr_special), whose selects
are about 28 issue slots a powr, six powrs in state 4c33d27bde9b894a. The rules can go where the chip's composition
already gives Metal's answers; powr_verdict.py reads that from these results.

lg2     MUFU.LG2(a) and MUFU.EX2(RRO.EX2(a))
narrow  RRO.EX2(a) and F2F.F16.F32(a): RRO's own output, and the half a NaN narrows to
fmul    FMUL.FTZ(a, s) and FMUL.FTZ(s, a), s each of FMUL_TABLE
base    the state's instructions on halves, x every half and y each of CHAIN_TABLE: F2F.F32.F16 of both, MUFU.LG2,
        FMUL.FTZ, RRO.EX2, MUFU.EX2, then F2F.F16.F32; the f32 result and the half
exponent the same with x each of CHAIN_TABLE and y every half

The first three run over UNARY_INPUTS (f32 bits: every half widened, every f32 exponent field, f32 near one, the 256
unorm8 texel values, NaN payloads). Texel (x, y) reads data word (y >> log2 rows) * WIDTH + x and table word
y & (rows - 1); its two results go out as raw bits through program.epilogue. Every instruction word is NAK's for the
state (powr_native_test holds them to it), and each variable-latency one is waited on through a barrier.
"""

import struct

from tools.benchloop import protocol
from tools.benchloop.sm53 import encode as e, operands, program, sph
from tools.benchloop.sm53.encode import Imm

WIDTH = 2048
LOG_WIDTH = WIDTH.bit_length() - 1
A, B, X, Y, T1, T2, T3 = 8, 9, 10, 11, 12, 13, 14
GPRS = 16
UNARY_WORDS = protocol.MAX_DATA // 4
PAIR_WORDS = 8192

CHAIN_TABLE = (
    0x0000, 0x8000, 0x0001, 0x8001, 0x03FF, 0x83FF, 0x0400, 0x3800, 0xB800, 0x3BFF, 0x3C00, 0x3C01,
    0xBC00, 0x4000, 0x7BFF, 0xFBFF, 0x7C00, 0xFC00, 0x7C01, 0x7E00, 0xFE00, 0x7FFF,
    0x0002, 0x3BFE, 0x3C02, 0xC000, 0x8400, 0x7BFE, 0x5BF8, 0x1C04, 0x3555, 0xFC01,
)  # fmt: skip
FMUL_TABLE = (0x00000000, 0x80000000, 0x7F800000, 0xFF800000, 0x7FFFFFFF, 0x00000001, 0x807FFFFF, 0x3F800000)
NAN_PAYLOADS = (0x7FC00000, 0xFFC00000, 0x7FFFFFFF, 0xFFFFFFFF, 0x7F800001, 0xFF800001, 0x7FBFFFFF, 0x7FFFE000)
NEAR_ONE = 64


def every_half() -> list[int]:
    """Give every half's f32 bits, a NaN with its sign and payload (Python 3.13's struct drops a half NaN's payload)."""
    out = []
    for h in range(1 << 16):
        if h & 0x7C00 == 0x7C00:
            out.append(h >> 15 << 31 | 0x7F800000 | (h & 0x3FF) << 13)
        else:
            out.append(struct.unpack('<I', struct.pack('<f', struct.unpack('<e', struct.pack('<H', h))[0]))[0])
    return out


def f32_extra() -> list[int]:
    """Give the f32 inputs no half widens to: every exponent field, the ulps around one, unorm8 texels, NaN payloads."""
    dense = [s | e << 23 | m for e in range(256) for m in (0, 1, 0x3FFFFF, 0x400000, 0x7FFFFF) for s in (0, 1 << 31)]
    near = [0x3F800000 + k for k in range(-NEAR_ONE, NEAR_ONE + 1) if k]
    texel = [struct.unpack('<I', struct.pack('<f', c / 255))[0] for c in range(256)]
    return dense + near + texel + list(NAN_PAYLOADS)


def unary_inputs() -> list[int]:
    return list(dict.fromkeys(every_half() + f32_extra()))


def _position() -> list[e.Instr]:
    return [
        e.ipa(4, 0x70).sched(wr_bar=0, delay=e.BARRIER_SET_CYCLES),
        e.f2i(4, 4, 'u32 rz').sched(wait=1, wr_bar=0, delay=1),
        e.ipa(5, 0x74).sched(wr_bar=1, delay=e.BARRIER_SET_CYCLES),
        e.f2i(5, 5, 'u32 rz').sched(wait=2, wr_bar=1, delay=e.BARRIER_SET_CYCLES),
        program.SEPARATOR,
    ]


def _operands(log_rows: int, table_bytes: int) -> list[e.Instr]:
    """Give A = data word (y >> log_rows) * WIDTH + x, after the table, and B = table word y & (rows - 1)."""
    safe = {'delay': program.SAFE_DELAY}
    return [
        e.shr(6, 5, Imm(log_rows)).sched(**safe),
        e.lop32i(7, 5, (1 << log_rows) - 1, 'and').sched(**safe),
        e.shl(6, 6, Imm(LOG_WIDTH)).sched(**safe),
        e.iadd(6, 6, 4).sched(**safe),
        e.shl(6, 6, Imm(2)).sched(**safe),
        e.shl(7, 7, Imm(2)).sched(**safe),
        e.ldc(A, 6, program.DATA_BANK, table_bytes).sched(wr_bar=0, delay=1),
        e.ldc(B, 7, program.DATA_BANK, 0).sched(wr_bar=0, delay=e.BARRIER_SET_CYCLES),
        program.SEPARATOR,
    ]


def _waited(*instrs: e.Instr) -> list[e.Instr]:
    """Give variable-latency instructions, each on barrier 0 and waited on before the next."""
    return [s for i in instrs for s in (i.sched(wr_bar=0, delay=e.BARRIER_SET_CYCLES), program.SEPARATOR)]


def widen(d: int, a: int) -> e.Instr:
    return e.f2f(d, a, 'dst=f32 src=f16')


def narrow(d: int, a: int) -> e.Instr:
    return e.f2f(d, a, 'dst=f16 src=f32')


def lg2(d: int, a: int) -> e.Instr:
    return e.mufu(d, a, 'lg2')


def ex2(d: int, a: int) -> e.Instr:
    return e.mufu(d, a, 'ex2')


def rro(d: int, a: int) -> e.Instr:
    return e.rro(d, a, 'ex2')


def fmul(d: int, a: int, b: int) -> e.Instr:
    return e.fmul(d, a, b, 'ftz')


def _chain(base: int, exponent: int) -> list[e.Instr]:
    return [
        *_waited(widen(T1, A), widen(T2, B), lg2(T3, base)),
        fmul(T3, T3, exponent).sched(delay=program.SAFE_DELAY),
        *_waited(rro(T3, T3), ex2(X, T3), narrow(Y, X)),
    ]


BODIES = {
    'lg2': lambda: _waited(lg2(X, A), rro(T1, A), ex2(Y, T1)),
    'narrow': lambda: _waited(rro(X, A), narrow(Y, A)),
    'fmul': lambda: [fmul(X, A, B).sched(delay=1), fmul(Y, B, A).sched(delay=program.SAFE_DELAY)],
    'base': lambda: _chain(T1, T2),
    'exponent': lambda: _chain(T2, T1),
}
# program: (its table, data words an experiment carries, whether its data are half bits)
LAYOUT = {
    'lg2': ((), UNARY_WORDS, False),
    'narrow': ((), UNARY_WORDS, False),
    'fmul': (FMUL_TABLE, PAIR_WORDS, False),
    'base': (CHAIN_TABLE, PAIR_WORDS, True),
    'exponent': (CHAIN_TABLE, PAIR_WORDS, True),
}


def rows_of(table: tuple[int, ...]) -> int:
    return max(1, len(table))


def code(name: str) -> bytes:
    table = LAYOUT[name][0]
    log_rows = rows_of(table).bit_length() - 1
    head = [*_position(), *_operands(log_rows, 4 * len(table))]
    return e.bundle(e.finish([*head, *BODIES[name](), program.SEPARATOR, *program.epilogue((X, Y))]))


def experiments(name: str) -> list[dict]:
    table, per, halves = LAYOUT[name]
    rows = rows_of(table)
    words = list(range(1 << 16)) if halves else unary_inputs()
    c = code(name)
    out = []
    for part, at in enumerate(range(0, len(words), per)):
        chunk = words[at : at + per]
        chunk += [chunk[0]] * (-len(chunk) % WIDTH)
        x = {
            'id': f'powr.{name}.{part}',
            'kind': 'readback',
            'target': {'format': 'rgba32float', 'width': WIDTH, 'height': len(chunk) // WIDTH * rows},
            'program': {
                'sph': protocol.b64(sph.pixel(operands.attributes(c))),
                'code': protocol.b64(c),
                'data': protocol.b64(struct.pack(f'<{len(table) + len(chunk)}I', *table, *chunk)),
                'gprs': GPRS,
            },
            'readback': 'full',
            'meta': {'family': 'powr-native', 'program': name, 'rows': rows, 'table': len(table)},
        }
        protocol.validate_experiment(x)
        out.append(x)
    return out


def generate() -> list[dict]:
    return [x for name in LAYOUT for x in experiments(name)]


def inputs(x: dict) -> list[tuple[int, int]]:
    """Give each texel's (data word, table word) in raster order; the table word is 0 where there is no table."""
    data = protocol.unb64(x['program']['data'])
    words = struct.unpack(f'<{len(data) // 4}I', data)
    rows, n = x['meta']['rows'], x['meta']['table']
    table, chunk = words[:n] or (0,), words[n:]
    width, height = x['target']['width'], x['target']['height']
    return [(chunk[y // rows * width + k], table[y % rows % len(table)]) for y in range(height) for k in range(width)]


def outputs(x: dict, result: dict | None) -> list[tuple[int, int]] | None:
    """Give each texel's two result words in raster order, or None when the experiment did not come back whole."""
    r = result or {}
    data = protocol.unb64((r.get('texels') or {}).get('data') or '')
    n = x['target']['width'] * x['target']['height']
    if r.get('status') != 'ok' or len(data) != 16 * n:
        return None
    return [program.unpack(list(t)) for t in struct.iter_unpack('<4I', data)]
