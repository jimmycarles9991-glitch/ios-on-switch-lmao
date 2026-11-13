"""BGRA8 fixed-function blend models, to fit against what the chip wrote.

`chip` is the model gpu.x1.blend_exact's device counts single out: on that probe's 8.4 M cases a pair it reproduces
every miss count against Metal's three references and every first miss, for all eight factor pairs. The source and its
alpha become 8-bit codes by the ROP's store rule, the factors are codes (1 - x is 255 - x), and S * Fs + D * Fd is
divided by 255 and rounded to nearest once. Rounding each product instead (`per_product`) fits the pairs with one
non-trivial product and misses SAT and XOR by millions. `apple` is Metal's half arithmetic on Apple GPUs.

A value is a float; a code is an int in [0, top]; factors are named as in FACTORS.
"""

import math
import struct
from collections.abc import Callable
from fractions import Fraction

FACTORS = ('zero', 'one', 'src_alpha', 'one_minus_src_alpha', 'dst_alpha', 'one_minus_dst_alpha')


def store(v: float, bits: int = 8) -> int:
    """The ROP's float -> unorm store: v truncated to bits + 4 fractional bits, then nearest with ties down."""
    top = (1 << bits) - 1
    if math.isnan(v) or v <= 0:
        return 0
    if v >= 1:
        return top
    u = math.floor(Fraction(v) * (1 << (bits + 4)))
    return (u * top + (1 << (bits + 3)) - 1) >> (bits + 4)


def nearest(v: float | Fraction, top: int = 255) -> int:
    """Metal's rule: clamp, then nearest with ties away from zero."""
    if isinstance(v, float) and math.isnan(v):
        return 0
    x = min(max(Fraction(v), Fraction(0)), Fraction(1)) * top
    return math.floor(x + Fraction(1, 2))


def half(x: float | Fraction) -> float:
    return struct.unpack('<e', struct.pack('<e', float(x)))[0]


def _code_factor(name: str, sa: int, da: int) -> int:
    return {
        'zero': 0,
        'one': 255,
        'src_alpha': sa,
        'one_minus_src_alpha': 255 - sa,
        'dst_alpha': da,
        'one_minus_dst_alpha': 255 - da,
    }[name]


def _value_factor(name: str, sa: Fraction, da: Fraction, one_minus: Callable[[Fraction], Fraction]) -> Fraction:
    return {
        'zero': Fraction(0),
        'one': Fraction(1),
        'src_alpha': sa,
        'one_minus_src_alpha': one_minus(sa),
        'dst_alpha': da,
        'one_minus_dst_alpha': one_minus(da),
    }[name]


def chip(s: float, sa: float, dst: int, da: int, fs: str, fd: str, quantise: Callable[[float], int] = store) -> int:
    """One channel: source value `s` with source alpha `sa`, destination code `dst` with destination alpha code `da`."""
    s8, sa8 = quantise(s), quantise(sa)
    t = s8 * _code_factor(fs, sa8, da) + dst * _code_factor(fd, sa8, da)
    return min(255, math.floor(Fraction(t, 255) + Fraction(1, 2)))


def per_product(s: float, sa: float, dst: int, da: int, fs: str, fd: str) -> int:
    s8, sa8 = store(s), store(sa)

    def mul(a: int, b: int) -> int:
        return math.floor(Fraction(a * b, 255) + Fraction(1, 2))

    return min(255, mul(s8, _code_factor(fs, sa8, da)) + mul(dst, _code_factor(fd, sa8, da)))


def real(s: float, sa: float, dst: int, da: int, fs: str, fd: str) -> int:
    """The exact result of the inputs as given, rounded once by Metal's rule."""
    sa_, da_ = Fraction(sa), Fraction(da, 255)
    f1 = _value_factor(fs, sa_, da_, lambda x: 1 - x)
    f2 = _value_factor(fd, sa_, da_, lambda x: 1 - x)
    return nearest(Fraction(s) * f1 + Fraction(dst, 255) * f2)


def apple(s: float, sa: float, dst: int, da: int, fs: str, fd: str, fused: bool = False) -> int:
    """Metal on Apple GPUs (X1Reference.halfSeparate / halfFMA): codes read as halves, 1 - x and each product in half."""
    sa_, da_ = Fraction(sa), Fraction(half(Fraction(da, 255)))
    f1 = _value_factor(fs, sa_, da_, lambda x: Fraction(half(1 - x)))
    f2 = _value_factor(fd, sa_, da_, lambda x: Fraction(half(1 - x)))
    d = Fraction(half(Fraction(dst, 255)))
    second = Fraction(half(d * f2))
    first = Fraction(s) * f1 if fused else Fraction(half(Fraction(s) * f1))
    return nearest(Fraction(half(first + second)))


MODELS: dict[str, Callable[[float, float, int, int, str, str], int]] = {
    'chip': chip,
    'per_product': per_product,
    'real': real,
    'apple': apple,
    'apple_fma': lambda s, sa, dst, da, fs, fd: apple(s, sa, dst, da, fs, fd, fused=True),
}


def fit(readings: list[tuple[float, float, int, int, str, str, int]]) -> dict[str, int]:
    """Per model, the readings it reproduces: each is (s, sa, dst, da, fs, fd, code the chip wrote)."""
    return {name: sum(model(*r[:6]) == r[6] for r in readings) for name, model in MODELS.items()}
