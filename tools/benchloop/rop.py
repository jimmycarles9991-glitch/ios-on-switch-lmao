"""How the ROP stores a float in a unorm or half channel.

Every code's value and the f32s around every rounding tie, every half in [0, 1] and the special values, written with
blending off, through an identity blend (ONE, ZERO) and through ONE, ONE over a zero destination, read back and held to
each rounding model of texels.ROUNDING. The models that match every texel are the answer.
"""

import math
import struct

from tools.benchloop import columns, protocol, texels

SPECIALS = (-1.0, -0.0, 0.0, 1.0, 1.0001, 2.0, math.inf, -math.inf, math.nan, 1e-45, 2.0**-126, 0.5)
TIE_ULPS = range(-6, 7)
CODE_ULPS = range(-2, 3)
WIDTH = protocol.MAX_SIDE
NOT_DRAWN = 0.125
FORMATS = ('bgra8unorm', 'rgba8unorm', 'rgb10a2unorm', 'rgba16float')
FORMATS16 = ('r16unorm', 'rgba16unorm')
# 16-bit codes: every SPARSE_STRIDE-th tie and code (65,535 of each is 850,000 values), plus every half in [0, 1]
SPARSE_STRIDE = 97

# SET_BLEND_STATE_PER_TARGET, SET_BLEND_PER_TARGET_*(0) (separate alpha, then colour and alpha op, source, destination)
# and SET_BLEND(0), with NV9097's OGL op and coefficient values
FUNC_ADD, ZERO, ONE = 0x8006, 0x4000, 0x4001
SET_BLEND_0 = 0x1360


def blend_methods(src: int, dst: int) -> list[list[int]]:
    per_target = [1, FUNC_ADD, src, dst, FUNC_ADD, src, dst]
    return [[0x12E4, 1], *([0x1E00 + 4 * k, v] for k, v in enumerate(per_target)), [SET_BLEND_0, 1]]


# name: (methods, restore, clear)
VARIANTS = {
    'noblend': ([], [], NOT_DRAWN),
    'one-zero': (blend_methods(ONE, ZERO), [[SET_BLEND_0, 0]], NOT_DRAWN),
    'one-one': (blend_methods(ONE, ONE), [[SET_BLEND_0, 0]], 0.0),
}


def nudged(x: float, ulps: int) -> float:
    bits = struct.unpack('<I', struct.pack('<f', x))[0] + ulps
    return struct.unpack('<f', struct.pack('<I', bits))[0]


def halves() -> list[float]:
    """Every half from 0 to 1."""
    return [struct.unpack('<e', struct.pack('<H', h))[0] for h in range(0x3C01)]


def values(bits: int) -> list[float]:
    """Give the f32s around each code and each tie between codes, every half in [0, 1] and the specials."""
    top = (1 << bits) - 1
    stride = SPARSE_STRIDE if bits > 10 else 1
    out = list(SPECIALS)
    for k in range(0, top, stride):
        tie = texels.f32((2 * k + 1) / (2 * top))
        out += [nudged(tie, u) for u in TIE_ULPS]
    for k in range(1, top, stride):
        out += [nudged(texels.f32(k / top), u) for u in CODE_ULPS]
    return out + (halves() if bits > 2 else [])


def half_values() -> list[float]:
    """Give the f32s around the midpoints of neighbouring halves across the half range, and the specials."""
    out = [*SPECIALS, 65504.0, 65519.0, 65520.0, -65520.0]
    for h in range(0, 0x7BFF, 7):
        lo, hi = (struct.unpack('<e', struct.pack('<H', c))[0] for c in (h, h + 1))
        mid = texels.f32((lo + hi) / 2)
        out += [nudged(mid, u) for u in (-1, 0, 1)] + [lo]
    return out


def lanes_of(fmt: str) -> list[list[float]]:
    """Give the values each RGBA lane carries: the colour lanes the colour format's, alpha its own."""
    if fmt == 'rgba16float':
        return [half_values()] * 4
    bits = texels.UNORM_BITS[fmt]
    rgb = values(bits[0])
    if len(bits) == 1:
        return [rgb, [0.0], [0.0], [0.0]]
    if bits[3] == bits[0]:
        return [rgb] * 4
    return [rgb, rgb, rgb, values(bits[3])]


def colours(fmt: str) -> list[tuple[float, float, float, float]]:
    lanes = lanes_of(fmt)
    if len(lanes[1]) == 1:
        return [(v, 0.0, 0.0, 1.0) for v in lanes[0]]
    if len({id(v) for v in lanes}) == 1:
        flat = lanes[0] + [0.0] * (-len(lanes[0]) % 4)
        return [tuple(flat[i : i + 4]) for i in range(0, len(flat), 4)]
    n = len(lanes[0]) // 3 + 1
    rgb = lanes[0] + [0.0] * (3 * n - len(lanes[0]))
    alpha = lanes[3]
    return [(rgb[3 * i], rgb[3 * i + 1], rgb[3 * i + 2], alpha[i % len(alpha)]) for i in range(n)]


def generate(formats: tuple[str, ...] = FORMATS, variants: tuple[str, ...] = tuple(VARIANTS)) -> list[dict]:
    out = []
    for fmt in formats:
        all_colours = colours(fmt)
        for variant in variants:
            methods, restore, clear = VARIANTS[variant]
            for part in range(0, len(all_colours), WIDTH):
                chunk = all_colours[part : part + WIDTH]
                e = {
                    'id': f'rop-{fmt}-{variant}-{part // WIDTH}',
                    'kind': 'readback',
                    'target': {'format': fmt, 'width': len(chunk), 'height': 1},
                    'program': columns.program(chunk),
                    'readback': 'full',
                    'clear': [clear] * 4,
                    'meta': {'family': 'rop', 'variant': variant, 'colours': chunk},
                }
                if methods:
                    e |= {'methods': methods, 'restore': restore}
                out.append(e)
    return out


def store(value: float, fmt: str, lane: int, model: str) -> int:
    """Give the code (unorm) or half bits (rgba16float) `model` says the ROP stores for `value`."""
    if fmt == 'rgba16float':
        return texels.half_bits(value, model)
    return texels.unorm(value, texels.UNORM_BITS[fmt][lane], model)


def stored(texel: bytes, fmt: str) -> tuple[int, ...]:
    if fmt == 'rgba16float':
        return struct.unpack('<4H', texel)
    return texels.codes(texel, fmt)


def models(fmt: str) -> tuple[str, ...]:
    return tuple(texels.HALF_ROUNDING) if fmt == 'rgba16float' else tuple(texels.ROUNDING)


def readings(experiment: dict, result: dict) -> list[tuple[float, int, int]]:
    """Give (value, lane, stored) for every value of the experiment that came back."""
    fmt = experiment['target']['format']
    data = (result.get('texels') or {}).get('data')
    if result.get('status') != 'ok' or not data:
        return []
    out = []
    for x, texel in enumerate(texels.split(protocol.unb64(data), fmt)):
        got = stored(texel, fmt)
        colour = experiment['meta']['colours'][x]
        out += [(colour[lane], lane, code) for lane, code in enumerate(got)]
    return out


def generate16() -> list[dict]:
    """Give the 16-bit unorm store sweep: R16 and RGBA16, blending off and ONE/ONE over zero."""
    return generate(FORMATS16, ('noblend', 'one-one'))


def check(experiment: dict, result: dict) -> dict:
    fmt = experiment['target']['format']
    row = {'id': experiment['id'], 'status': result.get('status'), 'checked': 0, 'models': {}}
    seen = readings(experiment, result)
    if not seen:
        return row | {'why': result.get('why', 'no texels came back')}
    clear = stored(texels.encode((experiment.get('clear') or [0.0])[0:1] * 4, fmt), fmt)
    if experiment.get('clear', [0])[0] != 0 and all(got == clear[lane] for _, lane, got in seen):
        return row | {'why': 'every texel is the clear colour: the draw never landed (a killed channel reads as ok)'}
    matches = dict.fromkeys(models(fmt), 0)
    first_miss: dict[str, dict] = {}
    for value, lane, got in seen:
        row['checked'] += 1
        for model in matches:
            want = store(value, fmt, lane, model)
            if want == got:
                matches[model] += 1
            elif model not in first_miss:
                first_miss[model] = {'value': repr(value), 'lane': lane, 'got': got, 'want': want}
    row['models'] = {m: {'matched': n, 'first_miss': first_miss.get(m)} for m, n in matches.items()}
    row['verdict'] = [m for m, n in matches.items() if n == row['checked']]
    return row
