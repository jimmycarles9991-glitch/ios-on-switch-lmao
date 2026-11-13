"""The instruction semantics of interp.py's single-lane Maxwell machine (sm50.rs field layouts)."""

import math
from fractions import Fraction

from tools.gpu.maxwell import isa, texture
from tools.gpu.maxwell.floats import F16, F32, RN, arith, convert, encode, flush, is_inf, is_nan, value
from tools.gpu.maxwell.interp import (
    MAX_STEPS,
    RZ_REG,
    Inputs,
    Lane,
    UnsupportedError,
    combine,
    f20,
    float_cmp,
    fmod,
    from_f32,
    half_op,
    halves,
    i20,
    int_cmp,
    mufu,
    operand_b,
    saturate,
)
from tools.gpu.maxwell.isa import field as bits

LANE_OPS = ('SHFL', 'FSWZADD', 'VOTE', 'ATOMS', 'STG', 'BAR')
# SULD and SUST coordinate registers by their dimension (sm50.rs set_image_dim): a 2D array's (x, y, layer) and a 3D
# image's (x, y, z); every other dimension's key keeps two words
SURFACE_COORDS = {4: 3, 5: 3}
FORMATS = {0: None, 1: F16, 2: F32}


def width_format(code: int) -> object:
    f = FORMATS.get(code)
    if f is None:
        raise UnsupportedError(f'float width {8 << code}')
    return f


def f2f(w: int, lane: Lane, name: str, inputs: Inputs) -> int:
    dst, src = width_format(bits(w, 8, 2)), width_format(bits(w, 10, 2))
    v = operand_b(lane, w, name, inputs, f20)
    if src is F16:
        v = (v >> 16) if bits(w, 41, 1) else v & 0xFFFF
    v = fmod(v, bits(w, 49, 1), bits(w, 45, 1), 16 if src is F16 else 32)
    if bits(w, 42, 1):
        if src is not dst:
            raise UnsupportedError('F2F to an integral value of another width')
        return integral(v, src, bits(w, 39, 2), bool(bits(w, 44, 1)))
    return convert(v, src, dst, bits(w, 39, 2), bool(bits(w, 44, 1)))


def integral(v: int, f: object, mode: int, ftz: bool) -> int:
    if ftz:
        v = flush(v, f)
    if is_nan(v, f):
        return f.nan
    if is_inf(v, f):
        return v
    x = value(v, f)
    n = {0: round(x), 1: x.__floor__(), 2: x.__ceil__(), 3: int(x)}[mode]
    return encode(Fraction(n), f, RN, bool(v & f.sign_bit))


def f2i(w: int, lane: Lane, name: str, inputs: Inputs) -> int:
    src = width_format(bits(w, 10, 2))
    v = fmod(operand_b(lane, w, name, inputs, f20), bits(w, 49, 1), bits(w, 45, 1))
    if src is F16 or bits(w, 8, 2) != 2:
        raise UnsupportedError('F2I other than f32 to 32 bits')
    signed = bool(bits(w, 12, 1))
    lo, hi = (-(1 << 31), (1 << 31) - 1) if signed else (0, (1 << 32) - 1)
    if is_nan(v, F32):
        return 0
    x = from_f32(v)
    mode = bits(w, 39, 2)
    r = {0: round, 1: math.floor, 2: math.ceil, 3: math.trunc}[mode](x) if math.isfinite(x) else (hi if x > 0 else lo)
    return max(lo, min(hi, r)) & 0xFFFFFFFF


def i2f(w: int, lane: Lane, name: str, inputs: Inputs) -> int:
    if bits(w, 10, 2) != 2:
        raise UnsupportedError('I2F from other than 32 bits')
    v = operand_b(lane, w, name, inputs, i20)
    if bits(w, 13, 1) and v & 0x80000000:
        v -= 1 << 32
    if bits(w, 45, 1):
        v = -v
    return encode(Fraction(v), width_format(bits(w, 8, 2)), bits(w, 39, 2))


def prmt(a: int, sel: int, c: int) -> int:
    src = a | (c << 32)
    out = 0
    for k in range(4):
        n = (sel >> (4 * k)) & 0xF
        byte = (src >> (8 * (n & 7))) & 0xFF
        if n & 8:
            byte = 0xFF if byte & 0x80 else 0
        out |= byte << (8 * k)
    return out


def alu(name: str, w: int, lane: Lane, inputs: Inputs) -> None:  # noqa: C901, PLR0912, PLR0915  one dispatch
    base = name.split('_', 1)[0]
    d, a = bits(w, 0, 8), lane.r(bits(w, 8, 8))
    if base == 'MOV32I':
        lane.w(d, bits(w, 20, 32))
    elif base == 'MOV':
        lane.w(d, operand_b(lane, w, name, inputs, i20))
    elif base in ('FADD', 'FMUL', 'FFMA', 'FMNMX', 'FSETP', 'FMUL32I', 'FADD32I'):
        fp32(base, name, w, lane, inputs, a, d)
    elif base in ('HADD2', 'HMUL2', 'HFMA2'):
        if name.endswith(('_32I', '_cbuf', '_imm')):
            raise UnsupportedError(name)
        lane.w(d, half_op(w, name, lane))
    elif base == 'F2F':
        lane.w(d, f2f(w, lane, name, inputs))
    elif base == 'F2I':
        lane.w(d, f2i(w, lane, name, inputs))
    elif base == 'I2F':
        lane.w(d, i2f(w, lane, name, inputs))
    elif base == 'SEL':
        lane.w(d, a if lane.p(bits(w, 39, 3), bits(w, 42, 1)) else operand_b(lane, w, name, inputs, i20))
    elif base == 'PRMT':
        if bits(w, 48, 3):
            raise UnsupportedError('PRMT mode')
        lane.w(d, prmt(a, operand_b(lane, w, name, inputs, i20), lane.r(bits(w, 39, 8))))
    elif base == 'ISETP':
        b = operand_b(lane, w, name, inputs, i20)
        r = int_cmp(bits(w, 49, 3), a, b, bool(bits(w, 48, 1)))
        lane.setp(bits(w, 3, 3), combine(bits(w, 45, 2), r, lane.p(bits(w, 39, 3), bits(w, 42, 1))))
    elif base == 'PSETP':
        x = lane.p(bits(w, 12, 3), bits(w, 15, 1))
        y = lane.p(bits(w, 29, 3), bits(w, 32, 1))
        z = lane.p(bits(w, 39, 3), bits(w, 42, 1))
        first = combine(bits(w, 24, 2), x, y)
        lane.setp(bits(w, 3, 3), combine(bits(w, 45, 2), first, z))
        lane.setp(bits(w, 0, 3), combine(bits(w, 45, 2), not first, z))
    elif base in ('LOP', 'LOP32I'):
        if base == 'LOP32I':
            x, y, op = fmod_not(a, bits(w, 55, 1)), fmod_not(bits(w, 20, 32), bits(w, 56, 1)), bits(w, 53, 2)
        else:
            x = fmod_not(a, bits(w, 39, 1))
            y = fmod_not(operand_b(lane, w, name, inputs, i20), bits(w, 40, 1))
            op = bits(w, 41, 2)
        lane.w(d, (x & y, x | y, x ^ y, y)[op])
    elif base in ('IADD', 'IADD32I'):
        iadd(base, name, w, lane, inputs, a, d)
    elif base in ('SHL', 'SHR'):
        s = operand_b(lane, w, name, inputs, i20)
        s = s & 31 if bits(w, 39, 1) else min(s, 32)
        if base == 'SHL':
            lane.w(d, a << s if s < 32 else 0)
        elif bits(w, 48, 1):
            sa = a - (1 << 32) if a & 0x80000000 else a
            lane.w(d, sa >> min(s, 31))
        else:
            lane.w(d, a >> s if s < 32 else 0)
    elif base == 'SHF':
        # the immediate's high bits are the type field's (bits 37-38): NAK reduces a shift below the type's width first
        shift = bits(w, 20, 17) if name.endswith('_imm') else lane.r(bits(w, 20, 8))
        lane.w(d, shf(name, w, a, lane.r(bits(w, 39, 8)), shift))
    elif base == 'IMNMX':
        b = operand_b(lane, w, name, inputs, i20)
        lt = int_cmp(1, a, b, bool(bits(w, 48, 1)))
        want_min = lane.p(bits(w, 39, 3), bits(w, 42, 1))
        lane.w(d, (a if lt else b) if want_min else (b if lt else a))
    elif base in ('IMUL', 'IMUL32I'):
        b = bits(w, 20, 32) if base == 'IMUL32I' else operand_b(lane, w, name, inputs, i20)
        high = bits(w, 53, 1) if base == 'IMUL32I' else bits(w, 39, 1)
        sa, sb = (
            (bits(w, 54, 2) if base == 'IMUL32I' else bits(w, 40, 2)) & 1,
            (bits(w, 55, 1) if base == 'IMUL32I' else bits(w, 41, 1)),
        )
        x = a - (1 << 32) if sa and a & 0x80000000 else a
        y = b - (1 << 32) if sb and b & 0x80000000 else b
        p = x * y
        lane.w(d, (p >> 32) if high else p)
    elif name == 'HSETP2_reg':
        hsetp2(w, lane)
    elif base in ('LOP3', 'IADD3', 'ISCADD'):
        lane.w(d, three_source(name, w, lane, inputs, a))
    elif name in ('XMAD_reg', 'XMAD_imm'):
        lane.w(d, xmad(name, w, lane, a))
    elif base == 'BFE':
        r = operand_b(lane, w, name, inputs, i20)
        lane.w(d, bfe(a, r & 0xFF, (r >> 8) & 0xFF, bool(bits(w, 48, 1)), bool(bits(w, 40, 1))))
    elif base == 'I2I':
        lane.w(d, i2i(w, operand_b(lane, w, name, inputs, i20)))
    elif base == 'FLO':
        v = fmod_not(operand_b(lane, w, name, inputs, i20), bits(w, 40, 1))
        if bits(w, 48, 1) and v & 0x80000000:
            v = ~v & 0xFFFFFFFF
        top = v.bit_length() - 1
        lane.w(d, 0xFFFFFFFF if v == 0 else (31 - top if bits(w, 41, 1) else top))
    elif base == 'MUFU':
        lane.w(d, mufu(bits(w, 20, 4), fmod(a, bits(w, 46, 1), bits(w, 48, 1))))
    elif base == 'RRO':
        lane.w(d, fmod(operand_b(lane, w, name, inputs, f20), bits(w, 49, 1), bits(w, 45, 1)))
    elif base == 'LDC':
        if bits(w, 44, 2):
            raise UnsupportedError('LDC mode')
        offset = (a + bits(w, 20, 16)) & 0xFFFFFFFF
        size = bits(w, 48, 3)
        for k in range({4: 1, 5: 2, 6: 4}.get(size, 0) or unsupported(f'LDC size {size}')):
            lane.w(d + k, inputs.cbuf(bits(w, 36, 5), offset + 4 * k))
    elif base == 'IPA':
        ipa(w, lane, inputs, d)
    elif base == 'LDL':
        at, count = local_words(w, lane)
        for k in range(count):
            lane.w(d + k, lane.local.get(at + 4 * k, 0))
    elif base == 'STL':
        at, count = local_words(w, lane)
        for k in range(count):
            lane.local[at + 4 * k] = lane.r(bits(w, 0, 8) + k)
    elif base == 'ALD':
        ald(w, lane, inputs, d)
    elif base == 'AST':
        ast(w, lane)
    elif base == 'TEX':
        texture.tex(name, w, lane, inputs, d)
    elif base == 'TEXS':
        texture.texs(w, lane, inputs)
    elif base == 'TLD':
        texture.tld(name, w, lane, inputs, d)
    elif base == 'TLDS':
        texture.tlds(w, lane, inputs)
    elif base == 'TXD':
        texture.txd(name, w, lane, inputs, d)
    elif name == 'TXQ_b':
        result = inputs.texture(('txq', lane.r(bits(w, 8, 8)), bits(w, 22, 6)))
        mask = bits(w, 31, 4)
        for k, c in enumerate(c for c in range(4) if mask >> c & 1):
            lane.w(d + k, result[c])
    elif base in ('S2R', 'CS2R'):
        lane.w(d, 0)
    elif base == 'PIXLD':
        lane.w(d, inputs.pixel(bits(w, 31, 3)))
    elif base in ('SULD', 'SUST', 'LDG', 'ATOM', 'RED', 'MEMBAR'):
        memory(base, w, lane, inputs, d)
    else:
        raise UnsupportedError(name)


def iadd(base: str, name: str, w: int, lane: Lane, inputs: Inputs, a: int, d: int) -> None:  # noqa: PLR0913, PLR0917  one opcode's operands
    """Execute IADD and IADD32I with their carry (sm50.rs: .X bit 43 or 53, .CC 47 or 52).

    A negated operand is its complement plus one; .X adds the carry in and complements instead (NAK's IAdd2X: bnot);
    .CC writes the 33rd bit.
    """
    if base == 'IADD32I':
        b, neg_a, neg_b, extend, write = bits(w, 20, 32), bits(w, 56, 1), 0, bits(w, 53, 1), bits(w, 52, 1)
    else:
        b = operand_b(lane, w, name, inputs, i20)
        neg_a, neg_b, extend, write = bits(w, 49, 1), bits(w, 48, 1), bits(w, 43, 1), bits(w, 47, 1)
    mask = 0xFFFFFFFF
    if extend:
        total = (a ^ mask if neg_a else a) + (b ^ mask if neg_b else b) + int(lane.carry)
    else:
        total = ((a ^ mask) + 1 if neg_a else a) + ((b ^ mask) + 1 if neg_b else b)
    if write:
        lane.carry = total > mask
    lane.w(d, total)


def memory(base: str, w: int, lane: Lane, inputs: Inputs, d: int) -> None:
    if base in {'MEMBAR', 'RED'}:
        return
    if base == 'ATOM':
        lane.w(d, 0)
        return
    dimension = 2 if base == 'LDG' else SURFACE_COORDS.get(bits(w, 33, 3), 2)
    coords = tuple(lane.r(bits(w, 8, 8) + k) for k in range(dimension))
    if base == 'LDG':
        count = {4: 1, 5: 2, 6: 4}.get(bits(w, 48, 3), 1)
        result = inputs.texture(('ldg', coords, bits(w, 20, 24)))
    else:
        handle = lane.r(bits(w, 39, 8))
        count = bits(w, 20, 4).bit_count() if not bits(w, 52, 1) else {4: 1, 5: 2, 6: 4}.get(bits(w, 20, 3), 1)
        if base == 'SUST':
            lane.stores.append((handle, coords, tuple(lane.r(bits(w, 0, 8) + k) for k in range(count))))
            return
        result = inputs.texture(('suld', handle, coords))
    for k in range(count):
        lane.w(d + k, result[k % 4])


def shf(name: str, w: int, low: int, high: int, shift: int) -> int:
    """Give SHF's funnel shift of high:low (NAK ir.rs OpShf's fold before SM70).

    A left shift gives the high word, a right one the word bit 48 picks; bits 37-38 are the type (0 32-bit, 2 U64, 3
    I64), bit 50 a wrapping shift count.
    """
    right = name.startswith('SHF_r')
    kind = bits(w, 37, 2)
    width = 64 if kind >= 2 else 32
    shift = shift & (width - 1) if bits(w, 50, 1) else min(shift, width)
    x = low | high << 32
    dst_high = bool(bits(w, 48, 1)) or not right
    if right and kind == 3:
        signed = x - (1 << 64) if x >> 63 else x
        shifted = (signed >> min(shift, 63)) & 0xFFFFFFFFFFFFFFFF
    elif right:
        shifted = x >> shift if shift < 64 else 0
    else:
        shifted = (x << shift) & 0xFFFFFFFFFFFFFFFF if shift < 64 else 0
    return shifted >> 32 if dst_high else shifted & 0xFFFFFFFF


def hsetp2(w: int, lane: Lane) -> None:
    """Run HSETP2's register form (Ryujinx InstHsetp2R's layout and semantics)."""
    a = halves(lane.r(bits(w, 8, 8)), bits(w, 47, 2))
    b = halves(lane.r(bits(w, 20, 8)), bits(w, 28, 2))
    results = []
    for k in range(2):
        x = fmod(a[k], bits(w, 44, 1), bits(w, 43, 1), 16)
        y = fmod(b[k], bits(w, 30, 1), bits(w, 31, 1), 16)
        if bits(w, 6, 1):
            x, y = flush(x, F16), flush(y, F16)
        results.append(float_cmp(bits(w, 35, 4), convert(x, F16, F32, 0, False), convert(y, F16, F32, 0, False)))
    if bits(w, 49, 1):
        both = results[0] and results[1]
        results = [both, not both]
    accum = lane.p(bits(w, 39, 3), bits(w, 42, 1))
    lane.setp(bits(w, 3, 3), combine(bits(w, 45, 2), results[0], accum))
    lane.setp(bits(w, 0, 3), combine(bits(w, 45, 2), results[1], accum))


def three_source(name: str, w: int, lane: Lane, inputs: Inputs, a: int) -> int:
    """Run LOP3.LUT, IADD3 and ISCADD (Ryujinx InstDecoders' layouts); not .X, .CC, predicates or IADD3's parts."""
    base, form = name.split('_', 1)
    b = operand_b(lane, w, name, inputs, i20)
    if base == 'ISCADD':
        mode = bits(w, 48, 2)
        if mode == 3 or bits(w, 47, 1):
            raise UnsupportedError('ISCADD .PO or .CC')
        x = -a if mode == 2 else a
        y = -b if mode == 1 else b
        return ((x << bits(w, 39, 5)) + y) & 0xFFFFFFFF
    c = lane.r(bits(w, 39, 8))
    if base == 'IADD3':
        if bits(w, 47, 2) or (form == 'reg' and bits(w, 31, 8)):
            raise UnsupportedError('IADD3 .X, .CC, part or shift')
        terms = [(-v if bits(w, 51 - k, 1) else v) for k, v in enumerate((a, b, c))]
        return sum(terms) & 0xFFFFFFFF
    lut = bits(w, 28, 8) if form == 'reg' else bits(w, 48, 8)
    flags = bits(w, 36, 3) if form == 'reg' else bits(w, 57 if form == 'imm' else 56, 1)
    if bits(w, 47, 1) or flags:
        raise UnsupportedError('LOP3 .X, .CC or predicate')
    out = 0
    for i in range(8):
        if lut >> i & 1:
            term = 0xFFFFFFFF
            for k, v in enumerate((a, b, c)):
                term &= v if i >> (2 - k) & 1 else ~v & 0xFFFFFFFF
            out |= term
    return out


def xmad(name: str, w: int, lane: Lane, a: int) -> int:
    """Run XMAD's register and immediate forms (nv50_ir emitXMAD's layout, yuzu's semantics); not .X, .CC or CSFU."""
    if bits(w, 38, 1) or bits(w, 47, 1) or bits(w, 50, 3) == 3:
        raise UnsupportedError('XMAD .X, .CC or CSFU')
    b = bits(w, 20, 16) if name == 'XMAD_imm' else lane.r(bits(w, 20, 8))
    b_hi = 0 if name == 'XMAD_imm' else bits(w, 35, 1)
    c = lane.r(bits(w, 39, 8))

    def half(v: int, hi: int, signed: int) -> int:
        h = (v >> 16 if hi else v) & 0xFFFF
        return h - 0x10000 if signed and h & 0x8000 else h

    product = half(a, bits(w, 53, 1), bits(w, 48, 1)) * half(b, b_hi, bits(w, 49, 1))
    if bits(w, 36, 1):
        product <<= 16
    c = {0: c, 1: c & 0xFFFF, 2: c >> 16, 4: (b << 16) + c}.get(bits(w, 50, 3))
    if c is None:
        raise UnsupportedError('XMAD mode')
    out = (product + c) & 0xFFFFFFFF
    if bits(w, 37, 1):
        out = (out & 0xFFFF) | (b & 0xFFFF) << 16
    return out


def bfe(v: int, pos: int, length: int, signed: bool, reverse: bool) -> int:
    if reverse:
        v = int(f'{v:032b}'[::-1], 2)
    if length == 0 or pos >= 32:
        return 0xFFFFFFFF if signed and v & 0x80000000 and pos >= 32 else 0
    length = min(length, 32 - pos)
    out = (v >> pos) & ((1 << length) - 1)
    if signed and out >> (length - 1):
        out -= 1 << length
    return out & 0xFFFFFFFF


def i2i(w: int, v: int) -> int:
    src_bits, dst_bits = 8 << bits(w, 10, 2), 8 << bits(w, 8, 2)
    v &= (1 << src_bits) - 1
    if bits(w, 13, 1) and v >> (src_bits - 1):
        v -= 1 << src_bits
    if bits(w, 49, 1):
        v = abs(v)
    if bits(w, 45, 1):
        v = -v
    if bits(w, 50, 1):
        lo, hi = (-(1 << (dst_bits - 1)), (1 << (dst_bits - 1)) - 1) if bits(w, 12, 1) else (0, (1 << dst_bits) - 1)
        v = max(lo, min(hi, v))
    v &= (1 << dst_bits) - 1
    if bits(w, 12, 1) and v >> (dst_bits - 1):
        v -= 1 << dst_bits
    return v & 0xFFFFFFFF


def fmod_not(v: int, invert: int) -> int:
    return ~v & 0xFFFFFFFF if invert else v


def unsupported(what: str) -> int:
    raise UnsupportedError(what)


def fp32(base: str, name: str, w: int, lane: Lane, inputs: Inputs, a: int, d: int) -> None:  # noqa: PLR0913, PLR0917  one dispatch per f32 opcode
    if base == 'FADD32I':
        r = arith('add', [fmod(a, bits(w, 54, 1), bits(w, 56, 1)), bits(w, 20, 32)], F32, RN, bool(bits(w, 55, 1)))
        lane.w(d, r)
    elif base == 'FMUL32I':
        r = arith('mul', [a, bits(w, 20, 32)], F32, RN, bool(bits(w, 53, 1)), bool(bits(w, 54, 1)))
        lane.w(d, saturate(r, F32) if bits(w, 55, 1) else r)
    elif base == 'FADD':
        b = fmod(operand_b(lane, w, name, inputs, f20), bits(w, 49, 1), bits(w, 45, 1))
        r = arith('add', [fmod(a, bits(w, 46, 1), bits(w, 48, 1)), b], F32, bits(w, 39, 2), bool(bits(w, 44, 1)))
        lane.w(d, saturate(r, F32) if bits(w, 50, 1) else r)
    elif base == 'FMUL':
        b = operand_b(lane, w, name, inputs, f20)
        r = arith('mul', [a, b], F32, bits(w, 39, 2), bool(bits(w, 44, 1)), bool(bits(w, 45, 1)))
        r ^= 0x80000000 if bits(w, 48, 1) and not is_nan(r, F32) else 0
        lane.w(d, saturate(r, F32) if bits(w, 50, 1) else r)
    elif base == 'FFMA':
        b, c = ffma_operands(name, w, lane, inputs)
        if bits(w, 48, 1):
            a ^= 0x80000000
        if bits(w, 49, 1):
            c ^= 0x80000000
        r = arith('fma', [a, b, c], F32, bits(w, 51, 2), bool(bits(w, 53, 1)), bool(bits(w, 54, 1)))
        lane.w(d, saturate(r, F32) if bits(w, 50, 1) else r)
    elif base == 'FMNMX':
        b = fmod(operand_b(lane, w, name, inputs, f20), bits(w, 49, 1), bits(w, 45, 1))
        x = fmod(a, bits(w, 46, 1), bits(w, 48, 1))
        lane.w(d, fminmax(x, b, lane.p(bits(w, 39, 3), bits(w, 42, 1)), bool(bits(w, 44, 1))))
    else:
        b = fmod(operand_b(lane, w, name, inputs, f20), bits(w, 44, 1), bits(w, 6, 1))
        x = fmod(a, bits(w, 7, 1), bits(w, 43, 1))
        if bits(w, 47, 1):
            x, b = flush(x, F32), flush(b, F32)
        r = float_cmp(bits(w, 48, 4), x, b)
        lane.setp(bits(w, 3, 3), combine(bits(w, 45, 2), r, lane.p(bits(w, 39, 3), bits(w, 42, 1))))


def ffma_operands(name: str, w: int, lane: Lane, inputs: Inputs) -> tuple[int, int]:
    """Give FFMA's B and C: maxwell.inc's _cr reads B from the constant buffer, _rc reads C from it."""
    if name.endswith('_rc'):
        return lane.r(bits(w, 39, 8)), inputs.cbuf(bits(w, 34, 5), bits(w, 20, 14) * 4)
    if name.endswith('_cr'):
        return inputs.cbuf(bits(w, 34, 5), bits(w, 20, 14) * 4), lane.r(bits(w, 39, 8))
    return operand_b(lane, w, name, inputs, f20), lane.r(bits(w, 39, 8))


def fminmax(x: int, y: int, want_min: bool, ftz: bool) -> int:
    if ftz:
        x, y = flush(x, F32), flush(y, F32)
    if is_nan(x, F32) and is_nan(y, F32):
        return F32.nan
    if is_nan(x, F32):
        return y
    if is_nan(y, F32):
        return x
    fx, fy = from_f32(x), from_f32(y)
    if fx == fy:
        neg = bool(x & y & 0x80000000) if not want_min else bool((x | y) & 0x80000000)
        return (x & 0x7FFFFFFF) | (0x80000000 if neg else 0)
    return (x if fx < fy else y) if want_min else (x if fx > fy else y)


def ipa(w: int, lane: Lane, inputs: Inputs, d: int) -> None:
    """Interpolate an attribute: one value over a lane's pixel, so its centroid reads the centre's."""
    freq, loc = bits(w, 54, 2), bits(w, 52, 2)
    if loc > 1 or bits(w, 38, 1):
        raise UnsupportedError('IPA offset or indexed')
    v = inputs.attribute(bits(w, 28, 10))
    if freq == 1:
        v = arith('mul', [v, lane.r(bits(w, 20, 8))], F32)
    elif freq not in (0, 2):
        raise UnsupportedError('IPA state')
    lane.w(d, v)


def local_words(w: int, lane: Lane) -> tuple[int, int]:
    """LDL's and STL's address (Ra plus the signed offset in bits 20-43) and its count of words (bits 48-50)."""
    count = {4: 1, 5: 2, 6: 4}.get(bits(w, 48, 3))
    if count is None:
        raise UnsupportedError(f'local access of type {bits(w, 48, 3)}')
    offset = bits(w, 20, 24)
    if offset & 1 << 23:
        offset -= 1 << 24
    return (lane.r(bits(w, 8, 8)) + offset) & 0xFFFFFFFF, count


def attribute_address(w: int, lane: Lane) -> int:
    if bits(w, 31, 1) or bits(w, 39, 8) != RZ_REG:
        raise UnsupportedError('ALD or AST of a patch or another vertex')
    return (lane.r(bits(w, 8, 8)) + bits(w, 20, 10)) & 0x3FF


def ald(w: int, lane: Lane, inputs: Inputs, d: int) -> None:
    """ALD: attribute words at a[Ra + addr], the invocation's inputs, or its own outputs (bit 32) as AST wrote them."""
    at = attribute_address(w, lane)
    written = {a: v for kind, a, v in lane.stores if kind == 'ast'}
    for k in range(bits(w, 47, 2) + 1):
        a = at + 4 * k
        lane.w(d + k, written.get(a, 0) if bits(w, 32, 1) else inputs.attribute(a))


def ast(w: int, lane: Lane) -> None:
    """AST: registers from bits 0-7 on to the output attributes at a[Ra + addr], kept in the lane's stores in order."""
    at = attribute_address(w, lane)
    for k in range(bits(w, 47, 2) + 1):
        lane.stores.append(('ast', at + 4 * k, lane.r(bits(w, 0, 8) + k)))


def branch_target(pc: int, w: int) -> int:
    off = bits(w, 20, 24)
    if off & (1 << 23):
        off -= 1 << 24
    return pc + 8 + off


def run(code: bytes, inputs: Inputs, outputs: int, lane: Lane | None = None) -> tuple[list[int], bool]:
    """Run one invocation to EXIT; give its first `outputs` registers and whether it was killed.

    `lane`: the lane it runs in, whose stores (AST's, SUST's) the caller reads after.
    """
    lane = run_lane(code, inputs, lane=lane)
    return [lane.r(i) for i in range(outputs)], lane.killed


def run_lane(code: bytes, inputs: Inputs, hook: object = None, lane: Lane | None = None) -> Lane:  # noqa: C901, PLR0912  every control opcode here
    """Run one invocation to EXIT and give its lane.

    `hook` (stage.py) executes the instructions it claims (its `claims(name)`, then `execute(name, word, lane)`): a
    stage's attribute, system value and memory operations.
    """
    words = isa.words_of(code)
    lane = lane or Lane()
    pc, stack = 8, []
    while True:
        lane.steps += 1
        if lane.steps > MAX_STEPS:
            raise UnsupportedError('no EXIT')
        idx = pc // 8
        if idx % 4 == 0:
            pc += 8
            continue
        if idx >= len(words):
            raise UnsupportedError('ran past the code')
        w = words[idx]
        name = isa.name(w)
        claimed = hook is not None and hook.claims(name)
        if not claimed and (name.split('_', 1)[0] in LANE_OPS or name == 'UNKNOWN'):
            raise UnsupportedError(name)
        guard = lane.p(bits(w, 16, 3), bits(w, 19, 1))
        nxt = pc + 8
        if name in ('SSY', 'PBK', 'PCNT'):
            stack.append((name, branch_target(pc, w)))
        elif name == 'SYNC':
            if guard:
                nxt = stack.pop()[1]
        elif name in ('BRK', 'CONT'):
            if guard:
                want = 'PBK' if name == 'BRK' else 'PCNT'
                while stack and stack[-1][0] != want:
                    stack.pop()
                nxt = stack.pop()[1]
        elif name == 'BRA':
            if guard:
                nxt = branch_target(pc, w)
        elif name in ('EXIT', 'KIL'):
            if guard:
                if name == 'KIL':
                    lane.killed = True
                return lane
        elif name != 'NOP' and guard:
            if claimed:
                hook.execute(name, w, lane)
            else:
                alu(name, w, lane, inputs)
        pc = nxt
