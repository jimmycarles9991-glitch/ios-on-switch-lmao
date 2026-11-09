"""What the falcon analysis tracks per program point (flow.py).

Registers, the stack relative to the function's entry, special registers, and the accesses it records.
"""

from __future__ import annotations

from dataclasses import dataclass, field

from tools.falcon.values import Sym, Value, join

ARGS = range(10, 16)
CLOBBERED = range(9, 16)


@dataclass(frozen=True)
class Access:
    code: int  # address of the instruction
    kind: str  # ld, st, iord, iowr, xdld, xdst, xcld
    address: int | None  # DMEM or I/O address; for transfers the local (DMEM/IMEM) address
    width: int
    value: str  # loaded/stored/written value, or the transfer's external offset and base
    function: int


@dataclass
class State:
    regs: list[Value]
    sp: int | None = 0
    stack: dict[int, Value] = field(default_factory=dict)
    special: dict[str, Value] = field(default_factory=dict)

    def copy(self) -> State:
        return State(list(self.regs), self.sp, dict(self.stack), dict(self.special))

    def merge(self, other: State) -> bool:
        changed = False
        for i in range(16):
            v = join(self.regs[i], other.regs[i])
            changed |= v != self.regs[i]
            self.regs[i] = v
        sp = self.sp if self.sp == other.sp else None
        changed |= sp != self.sp
        self.sp = sp
        for table, theirs in ((self.stack, other.stack), (self.special, other.special)):
            for k in list(table):
                v = join(table[k], theirs.get(k))
                if v is None:
                    del table[k]
                    changed = True
                elif v != table[k]:
                    table[k] = v
                    changed = True
        return changed


def entry_state(args: tuple[Value, ...]) -> State:
    regs: list[Value] = [None] * 16
    for i, v in zip(ARGS, args, strict=True):
        regs[i] = v if v is not None else Sym(f'arg r{i}')
    return State(regs)
