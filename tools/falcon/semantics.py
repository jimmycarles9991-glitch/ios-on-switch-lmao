"""What each falcon instruction does to the analysis state (state.py), and the accesses it records.

Sized results go into the low bits of the destination (arith.rst); a load gives the whole register the loaded value,
as the firmware's code relies on (it clears or masks before using a narrow load).
"""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass
from typing import Protocol

from tools.falcon import isa
from tools.falcon.state import CLOBBERED, Access, State
from tools.falcon.values import MASK, Sym, Value, binary, extract, insert, partial, text

ALU = {'add', 'adc', 'sub', 'sbb', 'shl', 'shr', 'sar', 'and', 'or', 'xor', 'mulu', 'muls', 'div', 'mod', 'sext'}
ALU |= {'bset', 'bclr', 'btgl', 'xbit'}


class Recorder(Protocol):
    def record(self, access: Access) -> None: ...

    def note_entry(self, address: int) -> None: ...


@dataclass
class Step:
    prog: Recorder
    insn: isa.Insn
    s: State
    function: int
    record: bool

    @property
    def ops(self) -> tuple[isa.Operand, ...]:
        return self.insn.operands

    @property
    def width(self) -> int:
        return self.insn.width or 32

    def value(self, op: isa.Operand) -> Value:
        if isinstance(op, isa.Reg):
            return self.s.regs[op.n]
        if isinstance(op, isa.Imm):
            return op.value & MASK
        return None

    def put(self, op: isa.Operand, v: Value, sized: bool = True) -> None:
        if isinstance(op, isa.Reg):
            self.s.regs[op.n] = partial(self.s.regs[op.n], v, self.insn.width if sized else None)

    def address(self, mem: isa.Operand) -> Value:
        assert isinstance(mem, isa.Mem)
        base = self.s.sp if mem.base == isa.SP else self.value(mem.base)
        if base is None:
            return None
        index: Value = 0 if mem.index is None else self.s.regs[mem.index.n]
        scaled = binary('mulu', index, mem.scale) if mem.scale > 1 else index
        return binary('add', binary('add', base, scaled), mem.offset)

    def note(self, kind: str, where: Value, width: int, value: str) -> None:
        if self.record:
            at = where if isinstance(where, int) else None
            self.prog.record(Access(self.insn.address, kind, at, width, value, self.function))


def _mov(x: Step) -> None:
    dst, src = (*x.ops, None)[:2]
    if src is None:
        x.put(dst, None)
    elif isinstance(dst, isa.Special):
        v = x.value(src)
        x.s.special[dst.name] = v
        if dst.name == 'sp':
            x.s.sp, x.s.stack = None, {}
        if dst.name in ('iv0', 'iv1', 'tv') and isinstance(v, int):
            x.prog.note_entry(v)
    elif isinstance(src, isa.Special):
        x.put(dst, x.s.sp if src.name == 'sp' else x.s.special.get(src.name), sized=False)
    else:
        x.put(dst, x.value(src))


def _sethi(x: Step) -> None:
    old, imm = x.value(x.ops[0]), x.ops[1]
    assert isinstance(imm, isa.Imm)
    x.put(x.ops[0], (old & 0xFFFF) | imm.value if isinstance(old, int) else None, sized=False)


def _ld(x: Step) -> None:
    mem = x.ops[1]
    assert isinstance(mem, isa.Mem)
    where = x.address(mem)
    if mem.base == isa.SP:
        x.put(x.ops[0], x.s.stack.get(where) if isinstance(where, int) else None, sized=False)
        return
    x.put(x.ops[0], Sym(f'D[{where:#x}].{x.width}', (where, x.width)) if isinstance(where, int) else None, False)
    x.note('ld', where, x.width, '')


def _st(x: Step) -> None:
    mem, v = x.ops[0], x.value(x.ops[1])
    assert isinstance(mem, isa.Mem)
    where = x.address(mem)
    if mem.base != isa.SP:
        x.note('st', where, x.width, text(v))
    elif isinstance(where, int):
        x.s.stack[where] = v
    else:
        x.s.stack.clear()


def _iord(x: Step) -> None:
    where = x.address(x.ops[1])
    x.put(x.ops[0], Sym(f'I[{where:#x}]') if isinstance(where, int) else None, sized=False)
    x.note(x.insn.name, where, 32, '')


def _iowr(x: Step) -> None:
    x.note(x.insn.name, x.address(x.ops[0]), 32, text(x.value(x.ops[1])))


def _transfer(x: Step) -> None:
    """Xdld/xdst/xcld: external offset in the first operand, local address and size code in the second."""
    local, ext = x.value(x.ops[1]), x.value(x.ops[0])
    base = x.s.special.get('xcbase' if x.insn.name == 'xcld' else 'xdbase')
    where = local & 0xFFFF if isinstance(local, int) else None
    size = 4 << (local >> 16 & 7) if isinstance(local, int) and x.insn.name != 'xcld' else 256
    x.note(x.insn.name, where, size, f'ext {text(ext)} base {text(base)}')


def _alu(x: Step) -> None:
    ops = x.ops
    if len(ops) == 3 and not isinstance(ops[1], isa.Special):
        x.put(ops[0], binary(x.insn.name, x.value(ops[1]), x.value(ops[2]), x.width))
    elif len(ops) == 2 and isinstance(ops[0], isa.Reg):
        x.put(ops[0], binary(x.insn.name, x.value(ops[0]), x.value(ops[1]), x.width))
    elif ops and ops[0] == isa.SP and x.insn.name == 'add':
        delta = x.value(ops[1])
        x.s.sp = (x.s.sp + delta) & MASK if isinstance(x.s.sp, int) and isinstance(delta, int) else None
    elif ops and isinstance(ops[0], isa.Reg):
        x.put(ops[0], None, sized=False)


def _unary(x: Step) -> None:
    a = x.value(x.ops[-1])
    if isinstance(a, int):
        v: Value = {'not': ~a & MASK, 'neg': -a & MASK, 'hswap': ((a << 16) | (a >> 16)) & MASK}[x.insn.name]
    else:
        v = None if a is None else Sym(f'{x.insn.name}({text(a)})', a.origin)
    x.put(x.ops[0], v)


def _bitfield(x: Step) -> None:
    f = x.ops[2]
    assert isinstance(f, isa.Field)
    if x.insn.name == 'ins':
        x.put(x.ops[0], insert(x.value(x.ops[0]), x.value(x.ops[1]), f.low, f.size), sized=False)
    else:
        x.put(x.ops[0], extract(x.value(x.ops[1]), f.low, f.size, x.insn.name == 'extrs'), sized=False)


def _stack(x: Step) -> None:
    name = x.insn.name
    reg = x.ops[0]
    assert isinstance(reg, isa.Reg)
    count = 1 if name in ('push', 'pop') else reg.n + 1
    extra = x.ops[1].value if name == 'mpopadd' and isinstance(x.ops[1], isa.Imm) else 0
    delta = -4 * count if name in ('push', 'mpush') else 4 * count + extra
    x.s.sp = (x.s.sp + delta) & MASK if isinstance(x.s.sp, int) else None
    if name == 'pop':
        x.s.regs[reg.n] = None
    elif name in ('mpop', 'mpopadd'):
        for i in range(count):
            x.s.regs[i] = None


def _call(x: Step) -> None:
    for i in CLOBBERED:
        x.s.regs[i] = None


def _clobber(x: Step) -> None:
    if x.ops and isinstance(x.ops[0], isa.Reg):
        x.put(x.ops[0], None, sized=False)


HANDLERS: dict[str, Callable[[Step], None]] = {
    'mov': _mov,
    'sethi': _sethi,
    'clear': lambda x: x.put(x.ops[0], 0),
    'ld': _ld,
    'st': _st,
    'iord': _iord,
    'iords': _iord,
    'iowr': _iowr,
    'iowrs': _iowr,
    **dict.fromkeys(('xdld', 'xdst', 'xcld'), _transfer),
    **dict.fromkeys(ALU, _alu),
    **dict.fromkeys(('not', 'neg', 'hswap'), _unary),
    **dict.fromkeys(('extr', 'extrs', 'ins'), _bitfield),
    **dict.fromkeys(('push', 'pop', 'mpush', 'mpop', 'mpopadd'), _stack),
    'call': _call,
    'lcall': _call,
    'setf': _clobber,
    'setp': _clobber,
}


def step(prog: Recorder, insn: isa.Insn, s: State, function: int, record: bool) -> None:
    handler = HANDLERS.get(insn.name)
    if handler is not None:
        handler(Step(prog, insn, s, function, record))
