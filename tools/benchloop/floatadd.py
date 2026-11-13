"""How the ROP adds a float source to a float target under ONE, ONE, ADD.

An accumulator that adds its fetched colour to a term (RenderBox's RG32Float coverage) is the same draw with the term as
the source and the ROP's ONE, ONE, ADD blend, where the ROP's sum is the shader's: an fp32 add rounded to nearest even
(fp32 targets), and one rounding of the exact sum of the fp32 source and the fp16 destination (fp16 targets: the
shader sums in fp32 and its store rounds once). Each experiment fills a target with one destination per row, draws one
source per column over it, and reads every texel back; each model of MODELS is held to every lane. The inputs separate
the models: ties of the fp16 grid, sums that cancel, signed zeros, denormals of both widths, infinities and NaN.
"""

import math
import struct
from fractions import Fraction

from tools.benchloop import columns, protocol, texels

# MAXWELL_B methods: SET_BLEND(0) and target 0's per-target blend, OGL values (clb197.h)
SET_BLEND = 0x1360
PER_TARGET = 0x1E00  # SET_BLEND_PER_TARGET_SEPARATE_FOR_ALPHA(0); COLOR_OP .. ALPHA_DEST_COEFF follow at 4-byte steps
FUNC_ADD, ONE = 0x8006, 0x4001
SIDE = 32


def f32(x: float) -> float:
    return struct.unpack('<f', struct.pack('<f', x))[0]


def f16(x: float) -> float:
    """Round to fp16, nearest even, overflowing to infinity."""
    try:
        return struct.unpack('<e', struct.pack('<e', x))[0]
    except OverflowError:
        return math.copysign(math.inf, x)


def _exact(a: float, b: float) -> float | None:
    """Give the exact sum of two floats as a double where one holds it (every fp32 + fp16 sum), else None."""
    if not (math.isfinite(a) and math.isfinite(b)):
        return a + b
    s = Fraction(a) + Fraction(b)
    return float(s) if Fraction(float(s)) == s else None


def _flush32(x: float) -> float:
    return math.copysign(0.0, x) if x != 0 and abs(x) < 2.0**-126 else x


def fp32_nearest(src: float, dst: float) -> float:
    return f32(f32(src) + f32(dst))


def fp32_flushed(src: float, dst: float) -> float:
    return _flush32(f32(_flush32(f32(src)) + _flush32(f32(dst))))


def fp16_single(src: float, dst: float) -> float:
    """Sum the fp32 source and the fp16 destination exactly and round once to fp16 (the shader's)."""
    s = _exact(f32(src), f16(dst))
    return f16(s) if s is not None else f16(f32(src) + f16(dst))


def fp16_double(src: float, dst: float) -> float:
    """Round the source to fp16, then the fp16 sum."""
    s = _exact(f16(f32(src)), f16(dst))
    return f16(s) if s is not None else f16(f16(f32(src)) + f16(dst))


def fp16_via_fp32(src: float, dst: float) -> float:
    """Round the sum to fp32, then to fp16 (two roundings of the exact sum)."""
    return f16(f32(f32(src) + f16(dst)))


MODELS = {
    'rg32float': {'fp32_nearest': fp32_nearest, 'fp32_flushed': fp32_flushed},
    'rgba16float': {'fp16_single': fp16_single, 'fp16_double': fp16_double, 'fp16_via_fp32': fp16_via_fp32},
}
# The shader's own sum: an accumulator's draw can leave its fetch to the ROP only where the ROP adds alike
SHADER = {'rg32float': 'fp32_nearest', 'rgba16float': 'fp16_single'}


def _bits(x: float, fmt: str) -> int:
    if math.isnan(x):
        return -1
    return (
        struct.unpack('<I', struct.pack('<f', x))[0]
        if fmt == 'rg32float'
        else struct.unpack('<H', struct.pack('<e', x))[0]
    )


def values(fmt: str) -> tuple[list[float], list[float]]:
    """Give the sources (fp32) and destinations (the target's width), SIDE of each."""
    half_ulp = [2.0**-11 * (1 + k / 4) for k in range(4)]
    # fp32 sources fp16 rounds onto a tie of the sum with 1: one rounding and two part
    twice = [2.0**-11 + 2.0**-25, 2.0**-11 - 2.0**-25, 3 * 2.0**-12 + 2.0**-26]
    dst = [0.0, -0.0, 1.0, -1.0, 0.5, 2.0**-14, 2.0**-24, 1e-38, 65504.0, math.inf, -math.inf, math.nan]
    dst += [1.0 + 2.0**-10 * k for k in range(1, 9)] + [0.25 * k for k in range(1, 13)]
    src = [0.0, -0.0, 1.0, -1.0, 1.4e-45, -1.4e-45, 2.0**-126, 2.0**-25, 2.0**-24 * 1.5, 3e38, math.inf, math.nan]
    src += [f32(h) for h in half_ulp] + [f32(-h) for h in half_ulp] + [f32(t) for t in twice] + [f32(-t) for t in twice]
    src += [f32(1 / 3), f32(-1 / 3), f32(0.1), f32(-0.75)]
    return src[:SIDE], [texels.decode(texels.encode((d, d, d, d), fmt), fmt)[0] for d in dst[:SIDE]]


def methods() -> tuple[list[list[int]], list[list[int]]]:
    set_up = [[SET_BLEND, 1], [PER_TARGET, 1]]
    set_up += [[PER_TARGET + 4 * k, v] for k, v in enumerate((FUNC_ADD, ONE, ONE, FUNC_ADD, ONE, ONE), 1)]
    # blend.py's rule: each method a draw sets goes back to 0 after it, the per-target factors too
    return set_up, [[method, 0] for method, _ in set_up]


def generate(formats: tuple[str, ...] = tuple(MODELS)) -> list[dict]:
    out = []
    for fmt in formats:
        src, dst = values(fmt)
        fill = b''.join(texels.encode((dst[y],) * 4, fmt) for y in range(len(dst)) for _ in range(len(src)))
        set_up, restore = methods()
        out.append(
            {
                'id': f'floatadd-{fmt}',
                'kind': 'readback',
                'target': {'format': fmt, 'width': len(src), 'height': len(dst)},
                'fill': protocol.b64(fill),
                'program': columns.program([(s, s, s, s) for s in src]),
                'methods': set_up,
                'restore': restore,
                'readback': 'full',
                'meta': {'family': 'floatadd', 'src': src, 'dst': dst},
            }
        )
    return out


def check(experiment: dict, result: dict) -> dict:
    """Each model against every lane the target holds: the models that match all of them are the ROP's."""
    fmt = experiment['target']['format']
    src, dst = experiment['meta']['src'], experiment['meta']['dst']
    row = {'id': experiment['id'], 'status': result.get('status'), 'checked': 0}
    data = (result.get('texels') or {}).get('data')
    if result.get('status') != 'ok' or not data:
        return row | {'why': result.get('why', 'no texels came back')}
    lanes = texels.FLOATS[fmt][0]
    matched = dict.fromkeys(MODELS[fmt], 0)
    first_miss: dict[str, dict] = {}
    for index, texel in enumerate(texels.split(protocol.unb64(data), fmt)):
        x, y = index % len(src), index // len(src)
        got = texels.decode(texel, fmt)[:lanes]
        for lane in range(lanes):
            row['checked'] += 1
            for name, model in MODELS[fmt].items():
                want = model(src[x], dst[y])
                if _bits(want, fmt) == _bits(got[lane], fmt):
                    matched[name] += 1
                elif name not in first_miss:
                    first_miss[name] = {
                        'src': repr(src[x]),
                        'dst': repr(dst[y]),
                        'got': repr(got[lane]),
                        'want': repr(want),
                    }
    row['models'] = {m: {'matched': n, 'first_miss': first_miss.get(m)} for m, n in matched.items()}
    row['verdict'] = [m for m, n in matched.items() if n == row['checked']]
    row['shader_exact'] = SHADER[fmt] in row['verdict']
    return row
