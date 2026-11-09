"""Recursive descent and constant propagation over falcon code.

Code is found from entry points by following branches and calls (and constant targets of indirect ones). Each
function is analysed once per distinct argument tuple (r10-r15 at its call sites): registers hold constants or
symbolic values (values.py), joined at control-flow merges. A call clobbers r9-r15 and keeps r0-r8 (the firmware's
functions save r0..rN with mpush). Every data, I/O and transfer access whose address resolves is recorded with the
code address that makes it.
"""

from __future__ import annotations

from tools.falcon import isa
from tools.falcon.semantics import step
from tools.falcon.state import ARGS, Access, State, entry_state
from tools.falcon.values import Sym, Value, text

MAX_CONTEXTS = 64
BRANCHES = ('bra', 'braeq', 'brane', 'lbra')
CALLS = ('call', 'lcall')


def _is_arg(v: Value) -> bool:
    return isinstance(v, Sym) and v.text.startswith('arg ')


class Program:
    def __init__(self, code: bytes, entries: list[int]):
        self.code = code
        self.insns: dict[int, isa.Insn] = {}
        self.functions: set[int] = set()
        self.undecodable: set[int] = set()
        self.accesses: set[Access] = set()
        self.calls: set[tuple[int, int, tuple[str, ...]]] = set()  # (site, target, args)
        self._contexts: dict[int, set[tuple[Value, ...]]] = {}
        self._pending: list[tuple[int, tuple[Value, ...]]] = []
        for e in entries:
            self._add_function(e)
        self._run(entries)

    def _decode(self, address: int) -> isa.Insn | None:
        if address in self.insns:
            return self.insns[address]
        if address in self.undecodable or address >= len(self.code):
            return None
        insn = isa.decode(self.code, address, address)
        if insn is None:
            self.undecodable.add(address)
            return None
        self.insns[address] = insn
        return insn

    def _add_function(self, address: int) -> None:
        self.functions.add(address)
        self._contexts.setdefault(address, set())

    def successors(self, insn: isa.Insn) -> list[int]:
        out = [] if insn.ends else [insn.address + insn.size]
        if insn.name in BRANCHES:
            out += [o.address for o in insn.operands if isinstance(o, isa.Target)]
        return out

    def _run(self, entries: list[int]) -> None:
        work = self._pending = [(e, (None,) * 6) for e in entries]
        done: set[tuple[int, tuple[Value, ...]]] = set()
        while work:
            function, args = work.pop()
            if (function, args) in done:
                continue
            done.add((function, args))
            for target, found in self._analyse(function, args):
                contexts = self._contexts.setdefault(target, set())
                context = found if found in contexts or len(contexts) < MAX_CONTEXTS else (None,) * 6
                contexts.add(context)
                self._add_function(target)
                work.append((target, context))

    def _analyse(self, function: int, args: tuple[Value, ...]) -> list[tuple[int, tuple[Value, ...]]]:
        states: dict[int, State] = {function: entry_state(args)}
        work = [function]
        while work:
            a = work.pop()
            insn = self._decode(a)
            if insn is None:
                continue
            s = states[a].copy()
            self._step(insn, s, function, record=False)
            for succ in self.successors(insn):
                if succ not in states:
                    states[succ] = s.copy()
                    work.append(succ)
                elif states[succ].merge(s):
                    work.append(succ)
        calls = []
        for a in sorted(states):
            insn = self._decode(a)
            if insn is None:
                continue
            s = states[a].copy()
            target = self._call_target(insn, s)
            if target is not None:
                call_args = tuple(None if _is_arg(v) else v for v in (s.regs[i] for i in ARGS))
                self.calls.add((a, target, tuple(text(v) for v in call_args)))
                calls.append((target, call_args))
            self._step(insn, s, function, record=True)
        return calls

    def _call_target(self, insn: isa.Insn, s: State) -> int | None:
        if insn.name not in CALLS:
            return None
        op = insn.operands[0]
        if isinstance(op, isa.Target):
            return op.address
        if isinstance(op, isa.Reg) and isinstance(s.regs[op.n], int):
            return s.regs[op.n]
        return None

    def record(self, access: Access) -> None:
        self.accesses.add(access)

    def note_entry(self, address: int) -> None:
        if address not in self.functions:
            self._add_function(address)
            self._pending.append((address, (None,) * 6))

    def _step(self, insn: isa.Insn, s: State, function: int, record: bool) -> None:
        step(self, insn, s, function, record)
