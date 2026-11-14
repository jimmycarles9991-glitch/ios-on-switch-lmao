"""HSETP2 as NAK's sm50/hsetp2.rs (x1-nak-bench d79f3f17a) encodes it: Ryujinx InstHsetp2R's layout."""

from tools.benchloop.sm53.encode import (
    COMBINE,
    FLOAT_CMP,
    PT,
    SWIZZLE,
    Instr,
    Pred,
    _mods,
    _pick,
    _prefixed,
    _Word,
)


def hsetp2(dst: tuple[int, int], a: int, b: int, test: str, accum: int | Pred = PT) -> Instr:
    """Give P, Q = (a.half <cmp> b.half) <combine> accum for the low and the high half.

    `test` is the comparison then modifiers, e.g. 'lt xor ftz a=h1h1 abs_b'; 'h_and' sets P to both halves' result
    and Q to its complement.
    """
    cmp, *rest = test.split()
    swizzles = {*_prefixed('a', SWIZZLE), *_prefixed('b', SWIZZLE)}
    m = _mods(' '.join(rest), {'ftz', 'h_and', 'abs_a', 'neg_a', 'abs_b', 'neg_b', *COMBINE, *swizzles})
    p, q = dst
    w = _Word('HSETP2_reg').put(3, 3, p).put(0, 3, q).flag(6, 'ftz' in m).reg(8, a).reg(20, b)
    w = w.put(28, 2, _pick(m, _prefixed('b', SWIZZLE))).flag(30, 'abs_b' in m).flag(31, 'neg_b' in m)
    w = w.put(35, 4, FLOAT_CMP[cmp]).pred(39, accum).flag(43, 'neg_a' in m).flag(44, 'abs_a' in m)
    return w.put(45, 2, _pick(m, COMBINE)).put(47, 2, _pick(m, _prefixed('a', SWIZZLE))).flag(49, 'h_and' in m).instr()
