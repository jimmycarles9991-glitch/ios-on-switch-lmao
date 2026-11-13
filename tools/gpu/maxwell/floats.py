"""IEEE binary32/binary16 arithmetic by exact rationals, with the rounding modes and flushes Maxwell's ALU applies.

A NaN result is the canonical NaN of its width (NVIDIA's 0x7fffffff, 0x7fff). Rounding is exact for every mode, so a
fused multiply-add rounds once.
"""

from dataclasses import dataclass
from fractions import Fraction

RN, RM, RP, RZ = 0, 1, 2, 3


@dataclass(frozen=True)
class Format:
    mant: int  # explicit significand bits
    exp: int  # exponent bits

    @property
    def bias(self) -> int:
        return (1 << (self.exp - 1)) - 1

    @property
    def nan(self) -> int:
        return (1 << (self.exp + self.mant)) - 1  # all ones but the sign: 0x7fffffff, 0x7fff

    @property
    def inf(self) -> int:
        return ((1 << self.exp) - 1) << self.mant

    @property
    def sign_bit(self) -> int:
        return 1 << (self.exp + self.mant)


F32 = Format(23, 8)
F16 = Format(10, 5)


def is_nan(bits: int, f: Format) -> bool:
    return bits & ~f.sign_bit & (f.sign_bit - 1) > f.inf


def is_inf(bits: int, f: Format) -> bool:
    return bits & (f.sign_bit - 1) == f.inf


def is_denorm(bits: int, f: Format) -> bool:
    mag = bits & (f.sign_bit - 1)
    return 0 < mag < (1 << f.mant)


def flush(bits: int, f: Format) -> int:
    return bits & f.sign_bit if is_denorm(bits, f) else bits


def value(bits: int, f: Format) -> Fraction:
    """Give a finite encoding's exact value."""
    sign = -1 if bits & f.sign_bit else 1
    e = (bits >> f.mant) & ((1 << f.exp) - 1)
    m = bits & ((1 << f.mant) - 1)
    if e == 0:
        return sign * Fraction(m, 1 << (f.mant + f.bias - 1))
    return sign * Fraction((1 << f.mant) | m) * Fraction(2) ** (e - f.bias - f.mant)


def encode(x: Fraction, f: Format, mode: int = RN, negative_zero: bool = False) -> int:
    """Round an exact value to `f` by `mode`, with gradual underflow and overflow to infinity or the largest finite."""
    if x == 0:
        return f.sign_bit if negative_zero else 0
    sign = f.sign_bit if x < 0 else 0
    a = abs(x)
    e = a.numerator.bit_length() - a.denominator.bit_length()
    if Fraction(2) ** e > a:
        e -= 1
    e = max(e, 1 - f.bias)
    scale = Fraction(2) ** (f.mant - e)
    q = a * scale
    n, rem = divmod(q.numerator, q.denominator)
    frac = Fraction(rem, q.denominator)
    up = {
        RN: frac > Fraction(1, 2) or (frac == Fraction(1, 2) and n & 1),
        RZ: False,
        RM: bool(sign) and frac > 0,
        RP: not sign and frac > 0,
    }[mode]
    n += up
    if n >= 1 << (f.mant + 1):
        n >>= 1
        e += 1
    if n == 0:
        return sign
    if n < 1 << f.mant:
        return sign | n
    biased = e + f.bias
    if biased >= (1 << f.exp) - 1:
        to_inf = mode == RN or (mode == RP and not sign) or (mode == RM and sign)
        return sign | (f.inf if to_inf else f.inf - 1)
    return sign | (biased << f.mant) | (n - (1 << f.mant))


def arith(  # noqa: C901, PLR0911, PLR0912, PLR0913, PLR0917  IEEE's special cases, op by op
    op: str, xs: list[int], f: Format, mode: int = RN, ftz: bool = False, fmz: bool = False
) -> int:
    """Give add, mul or fma of encodings, IEEE with NVIDIA's canonical NaN; ftz flushes inputs and result."""
    if ftz:
        xs = [flush(x, f) for x in xs]
    if any(is_nan(x, f) for x in xs):
        return f.nan
    if op == 'mul' and fmz and any(x & (f.sign_bit - 1) == 0 for x in xs):
        return 0
    signs = [bool(x & f.sign_bit) for x in xs]
    infs = [is_inf(x, f) for x in xs]
    if op == 'add':
        if all(infs) and signs[0] != signs[1]:
            return f.nan
        if any(infs):
            return xs[infs.index(True)]
        v = value(xs[0], f) + value(xs[1], f)
        neg_zero = (signs[0] and signs[1]) or (v == 0 and mode == RM and signs[0] != signs[1])
    elif op == 'mul':
        zero = [x & (f.sign_bit - 1) == 0 for x in xs]
        if any(infs) and any(zero):
            return f.nan
        if any(infs):
            return f.inf | (f.sign_bit if signs[0] != signs[1] else 0)
        v = value(xs[0], f) * value(xs[1], f)
        neg_zero = signs[0] != signs[1]
    else:
        zero = [x & (f.sign_bit - 1) == 0 for x in xs[:2]]
        prod_sign = signs[0] != signs[1]
        if (infs[0] or infs[1]) and any(zero):
            return f.nan
        if infs[0] or infs[1]:
            if infs[2] and signs[2] != prod_sign:
                return f.nan
            return f.inf | (f.sign_bit if prod_sign else 0)
        if infs[2]:
            return xs[2]
        if fmz and any(zero):
            v = value(xs[2], f)
            neg_zero = signs[2]
        else:
            v = value(xs[0], f) * value(xs[1], f) + value(xs[2], f)
            neg_zero = (prod_sign and signs[2]) or (v == 0 and mode == RM and prod_sign != signs[2])
    out = encode(v, f, mode, neg_zero)
    return flush(out, f) if ftz else out


def convert(bits: int, src: Format, dst: Format, mode: int = RN, ftz: bool = False) -> int:
    if ftz:
        bits = flush(bits, src)
    if is_nan(bits, src):
        return dst.nan
    if is_inf(bits, src):
        return dst.inf | (dst.sign_bit if bits & src.sign_bit else 0)
    out = encode(value(bits, src), dst, mode, bool(bits & src.sign_bit))
    return flush(out, dst) if ftz else out
