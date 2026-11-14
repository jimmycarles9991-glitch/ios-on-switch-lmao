"""The unorm store with NAK's two-FFMA rounding in front of the ROP, and the RGBA16F store with none.

Each channel's value v becomes t = FFMA(v, top, 2^23) (round to nearest even: 2^23 plus v * top rounded to an integer)
and w = FFMA(t, c, -2^23 * c) with c = f32(1 / top), which the ROP stores as the code Metal asks for: v * top rounded
to nearest, ties away from zero, after clamping to [0, 1], NaN 0. An RGBA16F target takes v as computed and stores it
toward zero, finite overflow at 65504, infinities kept, every NaN as 0x7e00.

Inputs travel as f32 bit patterns: a texel's four base words in the shader data, and per row an integer step added
to them (the ulps around each tie and each code). Texel (x, y) reads data texel (y >> log2 rows) * width + x and adds
(y & rows - 1) - centre.
"""

import math
import struct
from fractions import Fraction

from tools.benchloop import protocol, sweeps, texels
from tools.benchloop.sm53 import encode as e, operands, program, sph
from tools.benchloop.sm53.encode import Imm

BIG = 1 << 23
WIDTH = 64
DATA_TEXELS = protocol.MAX_DATA // 16
SPECIAL_BITS = (
    0x7FFFFFFF,
    0xFFC00000,
    0x7F800001,
    0xFFFFFFFF,
    0x7F800000,
    0xFF800000,
    0x80000000,
    0xBF800000,
    0x3F800347,
    0x40000000,
    0x00000001,
    0x007FFFFF,
    0x00800000,
    0x477FE000,
    0x477FF000,
    0x477FFFFF,
    0x47800000,
    0x501502F9,
)
# format: the top code of each lane (0 for a half lane), the lanes it stores, every how many values of a sweep it takes
FORMATS = {
    'bgra8unorm': ((255,) * 4, 4, 1),
    'rgba8unorm': ((255,) * 4, 4, 1),
    'rgb10a2unorm': ((1023, 1023, 1023, 3), 4, 1),
    'rgba16unorm': ((65535,) * 4, 4, 1),
    'r16unorm': ((65535,) * 4, 1, 4),
    'rgba16float': ((0,) * 4, 4, 1),
}
# sweep: (rows, centre): the steps a base takes are -centre .. rows - 1 - centre
STEPS = {'ties': (16, 8), 'codes': (8, 4), 'list': (1, 0), 'mids': (4, 2)}


def bits_of(v: float) -> int:
    return struct.unpack('<I', struct.pack('<f', v))[0]


def float_of(b: int) -> float:
    return struct.unpack('<f', struct.pack('<I', b & 0xFFFFFFFF))[0]


def every_half() -> list[int]:
    """Give every half's f32 bits, a NaN with its sign and payload: Python 3.13's struct drops a half NaN's payload."""
    return [
        h >> 15 << 31 | 0x7F800000 | (h & 0x3FF) << 13
        if h & 0x7C00 == 0x7C00
        else bits_of(struct.unpack('<e', struct.pack('<H', h))[0])
        for h in range(1 << 16)
    ]


def bases(sweep: str, top: int, every: int = 1) -> list[int]:
    """Give the f32 bits a sweep starts from for a lane whose top code is `top` (0: a half lane).

    `every` thins the ties, codes and halves; the specials always stay.
    """
    if sweep == 'ties':
        return [bits_of(texels.f32((2 * k + 1) / (2 * top))) for k in range(top)][::every]
    if sweep == 'codes':
        return [bits_of(texels.f32(k / top)) for k in range(1, top)][::every]
    if sweep == 'mids':
        pairs = (struct.unpack('<2e', struct.pack('<2H', h, h + 1)) for h in range(0, 0x7BFF, 4))
        mids = [bits_of((a + b) / 2) for a, b in pairs]
        return mids + [m | 0x80000000 for m in mids]
    return every_half()[::every] + list(SPECIAL_BITS)


def sweeps_of(fmt: str) -> tuple[str, ...]:
    return ('list', 'mids') if fmt == 'rgba16float' else ('ties', 'codes', 'list')


def texel_bases(fmt: str, sweep: str) -> list[tuple[int, int, int, int]]:
    """Give the four base words of each data texel: the stored lanes' sweeps, each lane its own top's, cycled."""
    tops, lanes, every = FORMATS[fmt]
    per_lane = [bases(sweep, top, every) for top in tops[:lanes]]
    if lanes == 1:
        return [(b, 0, 0, 0) for b in per_lane[0]]
    if all(b == per_lane[0] for b in per_lane):
        flat = per_lane[0] + [0] * (-len(per_lane[0]) % 4)
        return [tuple(flat[i : i + 4]) for i in range(0, len(flat), 4)]
    n = -(-len(per_lane[0]) // 3)
    rgb = per_lane[0] + [0] * (3 * n - len(per_lane[0]))
    return [(*rgb[3 * i : 3 * i + 3], per_lane[3][i % len(per_lane[3])]) for i in range(n)]


def _constants(tops: tuple[int, ...]) -> list[e.Instr]:
    """Give R8..R14: the rgb top and alpha top, 2^23, then c and -2^23 c for rgb and for alpha."""
    words = [float(tops[0]), float(tops[3]), float(BIG)]
    for top in (tops[0], tops[3]):
        c = texels.f32(1 / top)
        words += [c, -BIG * c]
    return [e.mov32i(8 + i, e.f32(w)).sched(delay=1) for i, w in enumerate(words)]


def code(log_rows: int, centre: int, tops: tuple[int, ...]) -> bytes:
    safe = {'delay': program.SAFE_DELAY}
    rounds = tops[0] != 0
    head = [
        e.ipa(4, 0x70).sched(wr_bar=0, delay=e.BARRIER_SET_CYCLES),
        e.f2i(4, 4, 'u32 rz').sched(wait=1, wr_bar=0, delay=1),
        e.ipa(5, 0x74).sched(wr_bar=1, delay=e.BARRIER_SET_CYCLES),
        e.f2i(5, 5, 'u32 rz').sched(wait=2, wr_bar=1, delay=e.BARRIER_SET_CYCLES),
        *(_constants(tops) if rounds else []),
        program.SEPARATOR,
        e.shr(6, 5, Imm(log_rows)).sched(**safe),
        e.lop32i(7, 5, (1 << log_rows) - 1, 'and').sched(**safe),
        e.shl(6, 6, Imm(WIDTH.bit_length() - 1)).sched(**safe),
        e.iadd(6, 6, 4).sched(**safe),
        e.shl(6, 6, Imm(4)).sched(**safe),
        e.iadd32i(7, 7, -centre).sched(**safe),
        e.ldc(0, 6, program.DATA_BANK, 0, 5).sched(wr_bar=0, delay=1),
        e.ldc(2, 6, program.DATA_BANK, 8, 5).sched(wr_bar=0, delay=e.BARRIER_SET_CYCLES),
        program.SEPARATOR,
        *(e.iadd(r, r, 7).sched(**safe) for r in range(4)),
    ]
    for lane in range(4) if rounds else ():
        top, c = (9, 13) if lane == 3 else (8, 11)
        head += [e.ffma(lane, lane, top, 10).sched(**safe), e.ffma(lane, lane, c, c + 1).sched(**safe)]
    return e.bundle(e.finish(head))


def experiments(fmt: str) -> list[dict]:
    tops, lanes, _ = FORMATS[fmt]
    out = []
    for sweep in sweeps_of(fmt):
        rows, centre = STEPS[sweep]
        c = code(rows.bit_length() - 1, centre, tops)
        all_texels = texel_bases(fmt, sweep)
        for part in range(0, len(all_texels), DATA_TEXELS):
            chunk = all_texels[part : part + DATA_TEXELS]
            width = min(WIDTH, len(chunk))
            chunk += [(0, 0, 0, 0)] * (-len(chunk) % width)
            data = b''.join(struct.pack('<4I', *t) for t in chunk)
            x = {
                'id': f'store2fma.{fmt}.{sweep}.{part // DATA_TEXELS}',
                'kind': 'readback',
                'target': {'format': fmt, 'width': width, 'height': len(chunk) // width * rows},
                'program': {
                    'sph': protocol.b64(sph.pixel(operands.attributes(c))),
                    'code': protocol.b64(c),
                    'data': protocol.b64(data),
                    'gprs': 16,
                },
                'clear': [0.125] * 4,
                'readback': 'full',
                'meta': {'family': 'store2fma', 'sweep': sweep, 'rows': rows, 'centre': centre, 'lanes': lanes},
            }
            protocol.validate_experiment(x)
            out.append(x)
    return out


def generate() -> list[dict]:
    return [x for fmt in FORMATS for x in experiments(fmt)]


def inputs(x: dict) -> list[list[int]]:
    """Give the f32 bits each texel of the target gets in each lane, in raster order."""
    data = protocol.unb64(x['program']['data'])
    words = struct.unpack(f'<{len(data) // 4}I', data)
    rows, centre = x['meta']['rows'], x['meta']['centre']
    width, height = x['target']['width'], x['target']['height']
    out = []
    for y in range(height):
        at, step = (y // rows * width) * 4, y % rows - centre
        out += [[(words[at + 4 * k + lane] + step) & 0xFFFFFFFF for lane in range(4)] for k in range(width)]
    return out


def round_f32(x: Fraction) -> float:
    """Give x rounded to the nearest f32, ties to even, past the largest finite to infinity."""
    if x == 0:
        return 0.0
    a = abs(x)
    exp = max(a.numerator.bit_length() - a.denominator.bit_length(), -126)
    if Fraction(2) ** exp > a:
        exp = max(exp - 1, -126)
    quantum = Fraction(2) ** (exp - 23)
    n = texels.round_half_even(a / quantum)
    v = n * quantum
    if v >= 2**128:
        return math.copysign(math.inf, x)
    return float(v) if x > 0 else -float(v)


def ffma(a: float, b: float, c: float) -> float:
    if not all(math.isfinite(v) for v in (a, b, c)):
        return a * b + c
    return round_f32(Fraction(a) * Fraction(b) + Fraction(c))


def rounded(v: float, top: int) -> float:
    """Give the value the two FFMAs leave for the ROP."""
    c = texels.f32(1 / top)
    return ffma(ffma(v, top, BIG), c, -BIG * c)


def wanted(bits: int, top: int) -> int:
    """Give Metal's code for a unorm lane, or the half the chip must keep for a half lane (top 0)."""
    v = float_of(bits)
    return texels.half_bits(v, 'rtz') if top == 0 else sweeps.metal_unorm(v, top.bit_length())


def predicted(bits: int, top: int) -> int:
    """Give the code the two FFMAs and the ROP's measured store rule (texels.gm20b) make, or the half as stored."""
    v = float_of(bits)
    return texels.half_bits(v, 'rtz') if top == 0 else texels.unorm(rounded(v, top), top.bit_length(), 'gm20b')


def stored(texel: bytes, fmt: str) -> tuple[int, ...]:
    if fmt == 'rgba16float':
        return struct.unpack('<4H', texel)
    return texels.codes(texel, fmt)


def analyse(experiments: list[dict], results: dict[str, dict]) -> dict:
    """Give per format the values checked, those off Metal's rule, and those off the host's prediction."""
    out: dict[str, dict] = {}
    for x in experiments:
        if x['meta'].get('family') != 'store2fma':
            continue
        fmt = x['target']['format']
        tops, lanes, _ = FORMATS[fmt]
        row = out.setdefault(fmt, {'checked': 0, 'off_metal': 0, 'off_predicted': 0, 'first_off': [], 'missing': []})
        r = results.get(x['id']) or {}
        data = protocol.unb64((r.get('texels') or {}).get('data') or '')
        if r.get('status') != 'ok' or not data:
            row['missing'].append(x['id'])
            continue
        for words, texel in zip(inputs(x), texels.split(data, fmt), strict=True):
            got = stored(texel, fmt)
            for lane in range(lanes):
                row['checked'] += 1
                want = wanted(words[lane], tops[lane])
                row['off_predicted'] += got[lane] != predicted(words[lane], tops[lane])
                if got[lane] != want:
                    row['off_metal'] += 1
                    if len(row['first_off']) < 8:
                        row['first_off'].append({'bits': f'{words[lane]:#010x}', 'lane': lane, 'got': got[lane]})
    return out
