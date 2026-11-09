"""Falcon v5 (fuc5) instruction decoder.

Transcribed from envytools' ISA table (envydis/falcon.c, MIT) and its falcon docs (docs/hw/falcon/*.rst): the entries
a v5 falcon without the crypto coprocessor decodes, in the table's order (first match wins). Operand fields keep
envydis's names: r0 = bits 0-3, r1 = bits 12-15, r2 = bits 8-11, r3 = bits 20-23 of the little-endian instruction.
"""

from __future__ import annotations

from dataclasses import dataclass

from tools.falcon.tables import SIZED, UNSIZED

SIZES = {0x00: 8, 0x40: 16, 0x80: 32}
CONDITIONS = {
    0x08: 'c',
    0x09: 'o',
    0x0A: 's',
    0x0B: 'e',
    0x0C: 'a',
    0x0D: 'na',
    0x18: 'nc',
    0x19: 'no',
    0x1A: 'ns',
    0x1B: 'ne',
    0x1C: 'g',
    0x1D: 'le',
    0x1E: 'l',
    0x1F: 'ge',
}
FLAGS = {
    0x08: 'c',
    0x09: 'o',
    0x0A: 's',
    0x0B: 'z',
    0x10: 'ie0',
    0x11: 'ie1',
    0x12: 'ie2',
    0x14: 'is0',
    0x15: 'is1',
    0x16: 'is2',
    0x18: 'ta',
}
SPECIAL = {
    0: 'iv0',
    1: 'iv1',
    3: 'tv',
    4: 'sp',
    5: 'pc',
    6: 'xcbase',
    7: 'xdbase',
    8: 'flags',
    0xB: 'xtargets',
    0xC: 'tstatus',
}
UNCONDITIONAL = 0x0E


@dataclass(frozen=True)
class Reg:
    n: int

    def __str__(self) -> str:
        return f'$r{self.n}'


@dataclass(frozen=True)
class Special:
    name: str

    def __str__(self) -> str:
        return f'${self.name}'


@dataclass(frozen=True)
class Imm:
    value: int

    def __str__(self) -> str:
        return hex(self.value) if self.value >= 0 else f'-{hex(-self.value)}'


@dataclass(frozen=True)
class Field:
    low: int
    size: int

    def __str__(self) -> str:
        return f'{self.low}:{self.size}'


@dataclass(frozen=True)
class Cond:
    name: str

    def __str__(self) -> str:
        return self.name


@dataclass(frozen=True)
class Target:
    address: int

    def __str__(self) -> str:
        return f'{self.address:#x}'


SP = Special('sp')


@dataclass(frozen=True)
class Mem:
    space: str  # D: data, I: I/O
    base: Reg | Special
    index: Reg | None = None
    scale: int = 1
    offset: int = 0

    def __str__(self) -> str:
        inner = str(self.base)
        if self.index is not None:
            inner += f'+{self.index}' + (f'*{self.scale}' if self.scale > 1 else '')
        if self.offset:
            inner += f'+{self.offset:#x}'
        return f'{self.space}[{inner}]'


Operand = Reg | Special | Imm | Field | Cond | Target | Mem


@dataclass(frozen=True)
class Insn:
    address: int
    size: int
    name: str
    width: int | None
    operands: tuple[Operand, ...]
    ends: bool = False  # no fall-through: ret, iret, exit, unconditional branches, mpopret

    def __str__(self) -> str:
        width = f' b{self.width}' if self.width else ''
        return f'{self.name}{width} ' + ' '.join(str(o) for o in self.operands)


def bits(word: int, low: int, size: int, signed: bool = False) -> int:
    value = (word >> low) & ((1 << size) - 1)
    if signed and value >> (size - 1):
        value -= 1 << size
    return value


@dataclass(frozen=True)
class _Word:
    w: int
    pc: int
    sz: int  # operand size in bytes
    long: bool  # byte 0 bit 0: the 16-bit immediate form of an ol0 entry


_REGS = {'r0': 0, 'r1': 12, 'r2': 8, 'r3': 20}
_IMMEDIATES = {  # low bit, size, signed
    'i8': (16, 8, False),
    'i16': (16, 16, False),
    'i8s': (16, 8, True),
    'i16s': (16, 16, True),
    'fi8s': (8, 8, True),
    'fi16s': (8, 16, True),
    'fi24s': (8, 24, True),
    'fi24': (8, 24, False),
    'fi32': (8, 32, False),
    'strap': (8, 2, False),
}
_TARGETS = {  # low bit, size, relative; None: 8 or 16 bits by the long form
    'bt': (16, None, True),
    'sfbt': (24, 8, True),
    'lfbt': (24, 16, True),
    'sffbt': (32, 8, True),
    'lffbt': (32, 16, True),
    'abt': (16, None, False),
    'ct': (16, None, False),
    'fct': (8, 16, False),
    'llt': (8, 24, False),
}


def _memory(spec: str, c: _Word) -> Mem:
    r1, r2, r3 = Reg(bits(c.w, 12, 4)), Reg(bits(c.w, 8, 4)), Reg(bits(c.w, 20, 4))
    imm8 = bits(c.w, 16, 8)
    return {
        'Dri': lambda: Mem('D', r1, offset=imm8 * c.sz),
        'Dsp': lambda: Mem('D', SP, offset=imm8 * c.sz),
        'Drr': lambda: Mem('D', r1, r2, c.sz),
        'Drralt': lambda: Mem('D', r1, r3, c.sz),
        'Dspr': lambda: Mem('D', SP, r2, c.sz),
        'Dr': lambda: Mem('D', r1),
        'Ir': lambda: Mem('I', r1),
        'Irr': lambda: Mem('I', r1, r2, 4),
        'Iri': lambda: Mem('I', r1, offset=imm8 * 4),
    }[spec]()


def _operand(spec: str, c: _Word) -> Operand:  # noqa: PLR0911  one return per operand kind
    if spec in _REGS:
        return Reg(bits(c.w, _REGS[spec], 4))
    if spec in ('s1', 's2'):
        n = bits(c.w, 12 if spec == 's1' else 8, 4)
        return Special(SPECIAL.get(n, f'sr{n}'))
    if spec in ('sp', 'flags'):
        return Special(spec)
    if spec in _IMMEDIATES:
        return Imm(bits(c.w, *_IMMEDIATES[spec]))
    if spec in ('i', 'is', 'ih'):
        value = bits(c.w, 16, 16 if c.long else 8, spec == 'is')
        return Imm(value << 16 if spec == 'ih' else value)
    if spec in ('bitf8', 'bitf16'):
        return Field(bits(c.w, 16, 5), bits(c.w, 21, 3 if spec == 'bitf8' else 5) + 1)
    if spec == 'fl':
        n = bits(c.w, 16, 5)
        return Cond(f'p{n}' if n < 8 else FLAGS.get(n, f'flag{n:#x}'))
    if spec in _TARGETS:
        low, size, relative = _TARGETS[spec]
        value = bits(c.w, low, size or (16 if c.long else 8), relative)
        return Target((c.pc + value) & 0xFFFFFFFF if relative else value)
    return _memory(spec, c)


def _condition(w: int) -> Cond | None:
    """Return the branch condition in bits 8-12, None for the unconditional form; KeyError for unknown ones."""
    n = bits(w, 8, 5)
    if n == UNCONDITIONAL:
        return None
    if n < 8:
        return Cond(f'p{n}')
    if 0x10 <= n < 0x18:
        return Cond(f'not p{n - 0x10}')
    return Cond(CONDITIONS[n])


def decode(code: bytes, offset: int, address: int) -> Insn | None:
    """Decode the instruction at `code[offset:]` executing at `address`; None for an encoding the table lacks."""
    window = code[offset : offset + 6]
    if not window:
        return None
    w = int.from_bytes(window.ljust(6, b'\0'), 'little')
    op = w & 0xFF
    sized = op < 0xC0
    for value, mask, length, name, specs, terminal in SIZED if sized else UNSIZED:
        if w & mask != value:
            continue
        long = bool(op & 1)
        size = (4 if long else 3) if length == 'ol0' else int(length)
        if offset + size > len(code):
            return None
        width = SIZES[op & 0xC0] if sized and name not in ('lbra', 'lcall') and specs[0] != 'r0' else None
        word = _Word(w, address, (width or 32) // 8, long)
        operands: list[Operand] = []
        ends = terminal
        for spec in specs:
            if spec != 'cond':
                operands.append(_operand(spec, word))
                continue
            try:
                cond = _condition(w)
            except KeyError:
                return None
            if cond is None:
                ends = True
            else:
                operands.append(cond)
        return Insn(address, size, name, width, tuple(operands), ends)
    return None
