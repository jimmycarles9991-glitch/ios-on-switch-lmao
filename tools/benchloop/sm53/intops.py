"""XMAD, LOP3.LUT, IADD3 and ISCADD as NAK's sm50/xmad.rs and sm50/int3.rs encode them.

The layouts are Ryujinx's InstDecoders (InstXmad*, InstLop3*, InstIadd3*, InstIscadd*) and nv50_ir's emitXMAD, which
agree; no .X, .CC, predicate results or IADD3 parts, as NAK never writes them.
"""

from tools.benchloop.sm53.encode import Imm, Instr, _form, _mods, _pick, _Word

XMAD_C = {'clo': 1, 'chi': 2, 'cbcc': 4}
LUT_XOR3 = 0x96


def xmad(d: int, a: int, b: int | Imm, c: int, mods: str = '') -> Instr:
    """Give d = a.half * b.half (<< 16 with 'psl') + C, the halves picked by 'a_hi' and 'b_hi'.

    C is c whole, or with 'clo'/'chi' its low/high half, or with 'cbcc' c + (b << 16); 'mrg' puts b's low half in the
    result's high half. An immediate b is an unsigned 16-bit value and has no high half.
    """
    m = _mods(mods, {'a_hi', 'b_hi', 'psl', 'mrg', *XMAD_C})
    if isinstance(b, Imm):
        if 'b_hi' in m or not 0 <= b.value <= 0xFFFF:
            raise ValueError('XMAD: an immediate B is 16 bits with no high half')
        w = _Word('XMAD_imm').put(20, 16, b.value)
    else:
        w = _Word('XMAD_reg').reg(20, b).flag(35, 'b_hi' in m)
    w = w.reg(0, d).reg(8, a).reg(39, c).flag(36, 'psl' in m).flag(37, 'mrg' in m)
    return w.put(50, 3, _pick(m, XMAD_C)).flag(53, 'a_hi' in m).instr()


def lop3(d: int, a: int, b: int | Imm, c: int, lut: int) -> Instr:
    """Give d = LUT(a, b, c), the table indexed by a, b, c as bits 2, 1, 0 (a = 0xF0, b = 0xCC, c = 0xAA)."""
    w = _form('LOP3', b).reg(0, d).reg(8, a).reg(39, c)
    if isinstance(b, Imm):
        return w.put(48, 8, lut).instr()
    return w.put(28, 8, lut).put(48, 3, 7).instr()


def iadd3(d: int, a: int, b: int | Imm, c: int, mods: str = '') -> Instr:
    m = _mods(mods, {'neg_a', 'neg_b', 'neg_c'})
    w = _form('IADD3', b).reg(0, d).reg(8, a).reg(39, c)
    return w.flag(49, 'neg_c' in m).flag(50, 'neg_b' in m).flag(51, 'neg_a' in m).instr()


def iscadd(d: int, a: int, b: int | Imm, shift: int, mods: str = '') -> Instr:
    """Give d = (a << shift) + b, either source negated."""
    m = _mods(mods, {'neg_a', 'neg_b'})
    if m == {'neg_a', 'neg_b'}:
        raise ValueError('ISCADD: one negated source at most')
    w = _form('ISCADD', b).reg(0, d).reg(8, a).put(39, 5, shift)
    return w.flag(48, 'neg_b' in m).flag(49, 'neg_a' in m).instr()
