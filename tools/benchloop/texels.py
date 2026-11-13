"""Target texels in the formats the bench renders: RGBA floats to bytes as a model of the store, and bytes back."""

import math
import struct
from collections.abc import Callable
from fractions import Fraction

from tools.benchloop import protocol

Colour = tuple[float, float, float, float]
FLOATS = {'rgba16float': (4, '<4e'), 'rgba32float': (4, '<4f'), 'r32float': (1, '<f'), 'rg32float': (2, '<2f')}
UNORM_BITS = {
    'bgra8unorm': (8, 8, 8, 8),
    'rgba8unorm': (8, 8, 8, 8),
    'rgb10a2unorm': (10, 10, 10, 2),
    'bgr10a2unorm': (10, 10, 10, 2),
    'r8unorm': (8,),
    'rg8unorm': (8, 8),
    'r16unorm': (16,),
    'rg16unorm': (16, 16),
    'rgba16unorm': (16, 16, 16, 16),
}
# codes as stored: the same bytes as their linear twins, the values sRGB-encoded
SRGB = {'rgba8unorm_srgb': 'rgba8unorm', 'bgra8unorm_srgb': 'bgra8unorm'}


def round_half_even(x: Fraction) -> int:
    floor = math.floor(x)
    rest = x - floor
    return floor + (rest > Fraction(1, 2) or (rest == Fraction(1, 2) and floor % 2 == 1))


def ties_down(x: Fraction) -> int:
    return math.ceil(x - Fraction(1, 2))


def gm20b(v: float, top: int) -> int:
    """GM20B's store (measured): v truncated to n + 4 fractional bits, then rounded to nearest with ties down."""
    shift = top.bit_length() + 4
    u = math.floor(Fraction(v) * (1 << shift))
    return (u * top + (1 << (shift - 1)) - 1) >> shift


# float -> unorm code models the ROP experiments tell apart: each takes the f32's exact value and the code's maximum.
# Exactly, v * top only ties at v = 0.5; the f32_mul_ models round the product to f32 first, which makes more ties.
ROUNDING: dict[str, Callable[[float, int], int]] = {
    'nearest': lambda v, top: round_half_even(Fraction(v) * top),
    'nearest_ties_down': lambda v, top: ties_down(Fraction(v) * top),
    'truncate': lambda v, top: math.floor(Fraction(v) * top),
    'f32_mul_add_half': lambda v, top: math.floor(f32(f32(v * top) + 0.5)),
    'f32_mul_even': lambda v, top: round_half_even(Fraction(f32(v * top))),
    'f32_mul_ties_down': lambda v, top: ties_down(Fraction(f32(v * top))),
    'gm20b': gm20b,
}
# RGB10A2's 2-bit alpha is the top two bits of the 10-bit code
NARROW_FROM = {2: 10}


def _half_rne(v: float) -> int:
    try:
        return struct.unpack('<H', struct.pack('<e', v))[0]
    except OverflowError:
        return 0xFC00 if v < 0 else 0x7C00


def _half_rtz(v: float) -> int:
    h = _half_rne(v)
    if h & 0x7FFF >= 0x7C00:
        return (h & 0x8000) | 0x7BFF
    if abs(struct.unpack('<e', struct.pack('<H', h))[0]) > abs(v):
        h -= 1
    return h


# f32 -> half models for an RGBA16F target: round to nearest even, or toward zero (largest finite on overflow)
HALF_ROUNDING: dict[str, Callable[[float], int]] = {'rne': _half_rne, 'rtz': _half_rtz}


def half_bits(value: float, model: str) -> int:
    v = f32(value)
    if math.isnan(v):
        return 0x7E00
    if math.isinf(v):
        return 0xFC00 if v < 0 else 0x7C00
    return HALF_ROUNDING[model](v)


def f32(x: float) -> float:
    return struct.unpack('<f', struct.pack('<f', x))[0] if math.isfinite(x) and abs(x) < 3.4e38 else x


def unorm(v: float, bits: int, rounding: str = 'nearest') -> int:
    top = (1 << bits) - 1
    if math.isnan(v) or v <= 0:
        return 0
    if v >= 1:
        return top
    if rounding == 'gm20b' and bits in NARROW_FROM:
        wide = NARROW_FROM[bits]
        return unorm(v, wide, rounding) >> (wide - bits)
    return min(top, ROUNDING[rounding](f32(v), top))


def encode(colour: Colour, fmt: str, rounding: str = 'nearest') -> bytes:
    if fmt in UNORM_BITS:
        bits = UNORM_BITS[fmt]
        codes = [unorm(c, n, rounding) for c, n in zip(colour[: len(bits)], bits, strict=True)]
        if fmt == 'bgra8unorm':
            return bytes([codes[2], codes[1], codes[0], codes[3]])
        if fmt in ('rgba8unorm', 'r8unorm', 'rg8unorm'):
            return bytes(codes)
        if bits[0] == 16:
            return struct.pack(f'<{len(codes)}H', *codes)
        return struct.pack('<I', codes[0] | codes[1] << 10 | codes[2] << 20 | codes[3] << 30)
    lanes, layout = FLOATS[fmt]
    return struct.pack(layout, *colour[:lanes])


def codes(texel: bytes, fmt: str) -> tuple[int, ...]:
    """Give a unorm texel's codes in RGBA order (as many as the format has)."""
    fmt = SRGB.get(fmt, fmt)
    if fmt in ('r8unorm', 'rg8unorm'):
        return tuple(texel)
    if fmt in ('r16unorm', 'rg16unorm', 'rgba16unorm'):
        return struct.unpack(f'<{len(texel) // 2}H', texel)
    if fmt == 'bgr10a2unorm':
        w = struct.unpack('<I', texel)[0]
        return w >> 20 & 0x3FF, w >> 10 & 0x3FF, w & 0x3FF, w >> 30
    if fmt == 'bgra8unorm':
        return texel[2], texel[1], texel[0], texel[3]
    if fmt == 'rgba8unorm':
        return tuple(texel)
    if fmt == 'rgb10a2unorm':
        w = struct.unpack('<I', texel)[0]
        return w & 0x3FF, w >> 10 & 0x3FF, w >> 20 & 0x3FF, w >> 30
    raise ValueError(f'{fmt} is not a unorm format')


def decode(texel: bytes, fmt: str) -> Colour:
    if fmt in UNORM_BITS:
        return tuple(c / ((1 << n) - 1) for c, n in zip(codes(texel, fmt), UNORM_BITS[fmt], strict=True))
    if fmt == 'rgba16float':
        return struct.unpack('<4e', texel)
    if fmt == 'rgba32float':
        return struct.unpack('<4f', texel)
    if fmt == 'r32float':
        return struct.unpack('<f', texel)[0], 0.0, 0.0, 1.0
    if fmt == 'rg32float':
        return (*struct.unpack('<2f', texel), 0.0, 1.0)
    raise ValueError(fmt)


def split(data: bytes, fmt: str) -> list[bytes]:
    size = protocol.FORMATS[fmt][1]
    return [data[i : i + size] for i in range(0, len(data), size)]
