"""Instruction shapes that keep a kext import off a function of ours with another signature.

names.py's sources can put one name on two Apple functions that only branch to the same place (kfree_type_impl on
kfree_type_var_impl, kfree_data on kfree_data_addr): a kext's call then reaches our function with its arguments in
the wrong registers. Two checks over the Apple kernel addresses the prelinked kexts use:
  - a straight-line tail call of Apple's and ours of the same name set the argument registers they both set alike,
    and branch to the same function where Apple's target has a name;
  - Apple functions bound to one function of ours are one function: the same instructions, PC-relative operands
    resolved.
"""

import bisect
import struct
from collections.abc import Callable

_HINTS = frozenset({
    0xD503201F,  # nop
    0xD503233F,  # paciasp
    0xD503237F,  # pacibsp
    0xD50323BF,  # autiasp
    0xD50323FF,  # autibsp
    0xD503241F,  # bti
    0xD503245F,  # bti c
    0xD503249F,  # bti j
    0xD50324DF,  # bti jc
})
_MAX_WRAPPER = 12
_ZR = 31


def _signed(value: int, bits: int) -> int:
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def _branch_target(word: int, pc: int) -> int:
    return pc + 4 * _signed(word & 0x3FFFFFF, 26)


def _logical_immediate(word: int) -> int:
    """The value of an AND/ORR (immediate) bitmask: N:imms picks the element size, immr rotates the run of ones."""
    n, immr, imms = (word >> 22) & 1, (word >> 16) & 0x3F, (word >> 10) & 0x3F
    width = 64 if word >> 31 else 32
    length = (n << 6 | (~imms & 0x3F)).bit_length() - 1
    size = 1 << length
    ones = (1 << ((imms & (size - 1)) + 1)) - 1
    rotate = immr & (size - 1)
    element = ((ones >> rotate) | (ones << (size - rotate))) & ((1 << size) - 1)
    value = 0
    for shift in range(0, width, size):
        value |= element << shift
    return value


def _register_value(regs: dict, index: int) -> tuple:
    return ('imm', 0) if index == _ZR else regs[index]


def _move(word: int, regs: dict) -> tuple | None:
    """The value MOVZ, MOVN or MOVK leaves in its register (low 32 bits: W and X writes of one constant agree)."""
    rd, imm, shift = word & 31, (word >> 5) & 0xFFFF, 16 * ((word >> 21) & 3)
    kind = word & 0x7F800000
    if kind == 0x52800000:
        return ('imm', (imm << shift) & 0xFFFFFFFF)
    if kind == 0x12800000:
        return ('imm', ~(imm << shift) & 0xFFFFFFFF)
    if kind == 0x72800000 and regs[rd][0] == 'imm':
        return ('imm', (regs[rd][1] & ~(0xFFFF << shift) | imm << shift) & 0xFFFFFFFF)
    return None


def _data_processing(word: int, regs: dict) -> tuple | None:
    """The value one register-writing instruction of a tail-call wrapper leaves, or None for any other instruction."""
    rn = (word >> 5) & 31
    if word & 0x7F800000 in (0x52800000, 0x12800000, 0x72800000):
        return _move(word, regs)
    if word & 0x7F200000 == 0x2A000000 and rn == _ZR and not (word >> 10) & 0x3F:
        return _register_value(regs, (word >> 16) & 31)
    if word & 0x7F800000 == 0x32000000:
        imm = _logical_immediate(word)
        return ('imm', imm & 0xFFFFFFFF) if rn == _ZR else ('orr', _register_value(regs, rn), imm)
    if word & 0x7F800000 == 0x12000000 and rn != _ZR:
        return ('and', regs[rn], _logical_immediate(word))
    if word & 0x9F000000 == 0x90000000:
        return ('global',)
    if word & 0x3F800000 == 0x11000000 and rn != _ZR:
        base = regs[rn]
        return base if base == ('global',) else ('sub' if word & 0x40000000 else 'add', base)
    if word & 0x3FC00000 == 0x39400000 and rn != _ZR:
        # A global's address and a value loaded from one are alike: which global it is cannot be compared across two
        # kernels (vm_map_unwire passes its sanitizer caller as &caller in 24B119 and through a pointer in ours).
        return base if (base := regs[rn]) == ('global',) else ('load', base)
    return None


def wrapper(words: list[int], pc: int) -> tuple[tuple, int] | None:
    """For straight-line code that ends in a tail call: what it puts in x0-x7 (None where the caller's value passes
    through) and the call's target. None for any other code."""
    regs = {i: ('arg', i) for i in range(31)}
    written = set()
    seen = 0
    for i, word in enumerate(words):
        if word in _HINTS:
            continue
        seen += 1
        if seen > _MAX_WRAPPER:
            return None
        if word & 0xFC000000 == 0x14000000:
            setup = tuple(regs[r] if r in written and regs[r] != ('arg', r) else None for r in range(8))
            return setup, _branch_target(word, pc + 4 * i)
        value = _data_processing(word, regs)
        if value is None:
            return None
        regs[word & 31] = value
        written.add(word & 31)
    return None


def setup_conflicts(apple: tuple, ours: tuple) -> list[int]:
    """The argument registers both wrappers set, to different values."""
    return [r for r, (a, o) in enumerate(zip(apple, ours, strict=True)) if a is not None and o is not None and a != o]


def code_identity(words: list[int], pc: int) -> tuple:
    """A function's instructions with PC-relative operands resolved, so two copies at different addresses compare
    equal."""
    out = []
    for i, word in enumerate(words):
        at = pc + 4 * i
        if word & 0x7C000000 == 0x14000000:
            out.append(('b', word >> 31, _branch_target(word, at)))
        elif word & 0x9F000000 == 0x90000000:
            page = (at & ~0xFFF) + (_signed(((word >> 29) & 3) | (((word >> 5) & 0x7FFFF) << 2), 21) << 12)
            out.append(('adrp', word & 31, page))
        elif word & 0x9F000000 == 0x10000000:
            out.append(('adr', word & 31, at + _signed(((word >> 29) & 3) | (((word >> 5) & 0x7FFFF) << 2), 21)))
        elif word & 0x3B000000 == 0x18000000:
            out.append(('literal', word & 0xC400001F, at + 4 * _signed((word >> 5) & 0x7FFFF, 19)))
        else:
            out.append(word)
    return tuple(out)


def read_words(data: bytes, offset: int, count: int) -> list[int]:
    return list(struct.unpack_from('<%dI' % count, data, offset))


class Code:
    """One image's function bodies: its bytes, an address-to-offset map and its sorted function starts."""

    def __init__(self, data: bytes, va2off: Callable[[int], int | None], starts: list[int]) -> None:
        self.data, self.va2off, self.starts = data, va2off, starts

    def body(self, address: int, limit: int = 0x400) -> list[int] | None:
        i = bisect.bisect_right(self.starts, address)
        end = self.starts[i] if i < len(self.starts) else address + 4
        offset = self.va2off(address)
        if offset is None:
            return None
        return read_words(self.data, offset, min(end - address, limit) // 4)


# Signature and call-graph names of functions no kext imports can be wrong without harm (an inlined callee's strings,
# a caller's call order), so a tail call is compared only against a target named another way.
WEAK_SOURCES = frozenset({'signature', 'callgraph'})


def check(apple: Code, ours: Code, checked: list[int], names: dict, oname: dict) -> list[tuple]:
    """Every binding of the Apple functions in checked whose shape is not our function's, and every function of ours
    bound from Apple functions that differ. names maps every named Apple address to (name, source)."""
    found = []
    by_ours = {}
    for address in sorted(checked):
        name = names[address][0]
        theirs, mine = apple.body(address), ours.body(oname[name])
        if not theirs or not mine:
            continue
        by_ours.setdefault(oname[name], []).append((address, theirs))
        a, o = wrapper(theirs, address), wrapper(mine, oname[name])
        if not (a and o):
            continue
        conflicts = setup_conflicts(a[0], o[0])
        if conflicts:
            found.append((hex(address), name, 'argument registers x%s differ' % ',x'.join(map(str, conflicts))))
        target = names.get(a[1])
        if target and target[1] not in WEAK_SOURCES and oname.get(target[0]) not in (None, o[1]):
            found.append((hex(address), name, 'tail call to %s, ours to %#x' % (target[0], o[1])))
    for group in by_ours.values():
        if len({code_identity(words, address) for address, words in group}) > 1:
            found.extend(
                (hex(address), names[address][0], 'one function of ours for %d different Apple functions' % len(group))
                for address, _ in group
            )
    return found
