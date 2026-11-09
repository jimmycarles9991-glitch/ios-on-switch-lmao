"""The value domain of the falcon constant propagation (flow.py).

A 32-bit constant, a symbolic value that remembers where it came from (a DMEM load, an I/O read, a function argument)
and what was done to it, or unknown (None).
"""

from __future__ import annotations

from dataclasses import dataclass

MASK = 0xFFFFFFFF
MAX_TEXT = 160


@dataclass(frozen=True)
class Sym:
    text: str
    origin: tuple[int, int] | None = None  # (DMEM address, width) of the load it derives from

    def __str__(self) -> str:
        return self.text


Value = int | Sym | None


def text(v: Value) -> str:
    if v is None:
        return '?'
    return hex(v) if isinstance(v, int) else v.text


def derive(fmt: str, *values: Value) -> Value:
    """Return the symbolic result of `fmt` over values none of which is unknown."""
    if any(v is None for v in values):
        return None
    out = fmt.format(*(text(v) for v in values))
    if len(out) > MAX_TEXT:
        return None
    origin = next((v.origin for v in values if isinstance(v, Sym) and v.origin), None)
    return Sym(out, origin)


def signed(v: int) -> int:
    return v - (1 << 32) if v & 0x80000000 else v


def _shift_right(a: int, b: int, arithmetic: bool) -> int:
    return (signed(a) >> (b & 31)) & MASK if arithmetic else a >> (b & 31)


def _sext(a: int, bit: int) -> int:
    bit &= 31
    if a >> bit & 1:
        return (a | (MASK << bit)) & MASK
    return a & ((1 << (bit + 1)) - 1)


BINARY = {
    'add': (lambda a, b: a + b, '({} + {})'),
    'adc': (None, '({} + {} + c)'),
    'sub': (lambda a, b: a - b, '({} - {})'),
    'sbb': (None, '({} - {} - c)'),
    'shl': (lambda a, b: a << (b & 31), '({} << {})'),
    'shr': (lambda a, b: _shift_right(a, b, False), '({} >> {})'),
    'sar': (lambda a, b: _shift_right(a, b, True), '({} >>s {})'),
    'and': (lambda a, b: a & b, '({} & {})'),
    'or': (lambda a, b: a | b, '({} | {})'),
    'xor': (lambda a, b: a ^ b, '({} ^ {})'),
    'mulu': (lambda a, b: (a & 0xFFFF) * (b & 0xFFFF), '({} * {})'),
    'muls': (lambda a, b: signed16(a) * signed16(b), '({} *s {})'),
    'div': (lambda a, b: a // b if b else None, '({} / {})'),
    'mod': (lambda a, b: a % b if b else None, '({} % {})'),
    'sext': (_sext, 'sext({}, {})'),
    'bset': (lambda a, b: a | 1 << (b & 31), '({} | bit {})'),
    'bclr': (lambda a, b: a & ~(1 << (b & 31)), '({} & ~bit {})'),
    'btgl': (lambda a, b: a ^ 1 << (b & 31), '({} ^ bit {})'),
    'xbit': (lambda a, b: a >> (b & 31) & 1, 'bit({}, {})'),
}


def signed16(v: int) -> int:
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def binary(name: str, a: Value, b: Value, width: int = 32) -> Value:
    fold, fmt = BINARY[name]
    if isinstance(a, int) and isinstance(b, int) and fold is not None:
        m = (1 << width) - 1
        r = fold(a & m if width < 32 else a, b & m if width < 32 else b)
        return None if r is None else r & MASK
    return derive(fmt, a, b)


def partial(old: Value, new: Value, width: int | None) -> Value:
    """Write a sized result into the low `width` bits of a register, keeping its high bits."""
    if width in (None, 32) or not isinstance(new, int):
        return new
    m = (1 << width) - 1
    if isinstance(old, int):
        return (old & ~m & MASK) | (new & m)
    return None


def extract(a: Value, low: int, size: int, signed_field: bool) -> Value:
    if isinstance(a, int):
        v = a >> low & ((1 << size) - 1)
        if signed_field and v >> (size - 1):
            v = (v - (1 << size)) & MASK
        return v
    return derive('{}[' + f'{low}:{size}' + ']', a)


def insert(dst: Value, src: Value, low: int, size: int) -> Value:
    if isinstance(dst, int) and isinstance(src, int):
        m = ((1 << size) - 1) << low
        return (dst & ~m & MASK) | (src << low & m)
    return derive('ins({}, {}, ' + f'{low}:{size}' + ')', dst, src)


def join(a: Value, b: Value) -> Value:
    return a if a == b else None
