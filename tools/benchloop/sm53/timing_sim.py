"""A cycle model of one Maxwell warp, the fake device's stand-in for the GPU's timing and its stale reads.

Instructions issue in order, each its predecessor's encoded delay after it. A fixed-latency result becomes visible
`latency` cycles after issue and nothing interlocks on it: an earlier read sees the old value, as on the hardware. A
variable-latency result lands the same way and also releases its write barrier then; a wait mask holds the issue
until every barrier it names is released (a read barrier releases a cycle after issue). Delay 0 issues the next
instruction in the same cycle when the pair is one the table allows (one pair at a time), else counts as 1. Register
operands of one instruction that share a bank (register % 4) cost `bank_penalty` cycles per extra read, unless the
previous instruction's reuse flag kept that slot's register in the operand cache.
"""

import heapq
import math
from collections import Counter
from dataclasses import dataclass, field

from tools.benchloop.sm53 import encode, operands, program
from tools.benchloop.sm53.encode import NO_BAR
from tools.benchloop.sm53.operands import Decoded
from tools.gpu.maxwell import execute
from tools.gpu.maxwell.interp import Lane

SCHEDULERS = 8  # GM20B: two SMs of four warp schedulers, each issuing one warp-instruction a cycle
WARP = 32
BARRIERS = 6
READ_RELEASE = 1


@dataclass(frozen=True)
class Table:
    """The made-up GPU: latencies and issue costs by operands.Decoded.key (a MUFU key falls back to 'MUFU')."""

    latency: dict[str, int]
    issue: dict[str, int] = field(default_factory=dict)
    dual: frozenset[tuple[str, str]] = frozenset()
    bank_penalty: int = 0
    default_latency: int = 6

    def latency_of(self, key: str) -> int:
        return self.latency.get(key, self.latency.get(key.partition('.')[0], self.default_latency))

    def issue_of(self, key: str) -> int:
        return self.issue.get(key, self.issue.get(key.partition('.')[0], 1))

    def pairs(self, first: Decoded, second: Decoded) -> bool:
        return first.control.delay == 0 and (first.base, second.base) in self.dual


@dataclass
class _Deferred(Lane):
    """A lane that reads the visible registers and queues its writes instead of making them."""

    queued: list = field(default_factory=list)

    def w(self, i: int, v: int) -> None:
        self.queued.append(('r', i, v))

    def setp(self, i: int, v: bool) -> None:
        self.queued.append(('p', i, v))


class _Banks:
    def __init__(self) -> None:
        self.cache: dict[int, int] = {}

    def conflicts(self, op: Decoded) -> int:
        reads = op.reads()
        fresh = {r for slot, r in reads.items() if self.cache.get(slot) != r}
        self.cache = {slot: r for slot, r in reads.items() if op.control.reuse >> slot & 1}
        return sum(n - 1 for n in Counter(r % 4 for r in fresh).values())


class _Warp:
    def __init__(self, table: Table, data: bytes) -> None:
        self.table = table
        self.inputs = program.inputs(data)
        self.lane = Lane()
        self.pending: list[tuple[int, int, str, int, int]] = []
        self.barriers = [0] * BARRIERS
        self.banks = _Banks()
        self.prev: tuple[Decoded, int, bool] | None = None
        self.seq = 0

    def issue_time(self, op: Decoded) -> tuple[int, bool]:
        """Give when `op` issues and whether it pairs with its predecessor."""
        t, paired = 0, False
        if self.prev is not None:
            before, at, was_paired = self.prev
            paired = self.table.pairs(before, op) and not was_paired
            t = at if paired else at + max(before.control.delay, 1)
        waits = [self.barriers[b] for b in range(BARRIERS) if op.control.wait >> b & 1]
        t = max(t, *waits) if waits else t
        return t + self.table.bank_penalty * self.banks.conflicts(op), paired

    def land(self, t: int) -> None:
        while self.pending and self.pending[0][0] <= t:
            _, _, kind, i, v = heapq.heappop(self.pending)
            if kind == 'r':
                self.lane.w(i, v)
            else:
                self.lane.setp(i, bool(v))

    def execute(self, op: Decoded, t: int) -> None:
        guard = self.lane.p(op.word >> 16 & 7, bool(op.word >> 19 & 1))
        if op.base != 'NOP' and guard:
            deferred = _Deferred(regs=self.lane.regs, preds=self.lane.preds)
            execute.alu(op.name, op.word, deferred, self.inputs)
            visible = t + self.table.latency_of(op.key)
            for kind, i, v in deferred.queued:
                self.seq += 1
                heapq.heappush(self.pending, (visible, self.seq, kind, i, int(v)))
        latency = self.table.latency_of(op.key)
        if op.control.wr_bar != NO_BAR:
            self.barriers[op.control.wr_bar] = max(self.barriers[op.control.wr_bar], t + latency)
        if op.control.rd_bar != NO_BAR:
            self.barriers[op.control.rd_bar] = max(self.barriers[op.control.rd_bar], t + READ_RELEASE)


def branch_target(index: int, word: int) -> int:
    """Give the instruction index a BRA at `index` jumps to."""
    offset = word >> 20 & 0xFFFFFF
    offset -= (offset & 1 << 23) << 1
    target = encode.address(index) + 8 + offset
    return target // 32 * 3 + (target % 32 - 8) // 8


def _taken(op: Decoded, lane: Lane) -> bool:
    return lane.p(op.word >> 16 & 7, bool(op.word >> 19 & 1))


def run(code: bytes, data: bytes, table: Table, outputs: int = program.OUTPUT_REGISTERS) -> tuple[list[int], int]:
    """Run one warp to EXIT; give its first `outputs` registers as EXIT reads them and the cycles it took."""
    warp = _Warp(table, data)
    ops = operands.decode(code)
    i = 0
    while i < len(ops):
        op = ops[i]
        t, paired = warp.issue_time(op)
        warp.land(t)
        if op.base == 'EXIT':
            return [warp.lane.r(k) for k in range(outputs)], t + 1
        if op.base == 'BRA':
            i = branch_target(i, op.word) if _taken(op, warp.lane) else i + 1
        else:
            warp.execute(op, t)
            i += 1
        warp.prev = (op, t, paired)
    raise ValueError('the code has no EXIT')


def path(code: bytes, data: bytes = b'') -> list[Decoded]:
    """Give the instructions one invocation issues up to EXIT, branches followed, every result visible at once."""
    ops, lane, inputs = operands.decode(code), Lane(), program.inputs(data)
    out, i = [], 0
    while ops[i].base != 'EXIT':
        op = ops[i]
        out.append(op)
        if len(out) > execute.MAX_STEPS:
            raise ValueError('no EXIT')
        if op.base == 'BRA':
            i = branch_target(i, op.word) if _taken(op, lane) else i + 1
            continue
        if op.base != 'NOP' and _taken(op, lane):
            execute.alu(op.name, op.word, lane, inputs)
        i += 1
    return out


def issue_cycles(code: bytes, table: Table, data: bytes = b'') -> int:
    """Give the scheduler cycles one warp occupies when other warps fill every delay: issue costs and bank conflicts."""
    total, banks, prev, paired = 0, _Banks(), None, False
    for op in path(code, data):
        cost = table.issue_of(op.key) + table.bank_penalty * banks.conflicts(op)
        paired = prev is not None and table.pairs(prev, op) and not paired
        total += 0 if paired else cost
        prev = op
    return total


def draw_ns(cycles: int, pixels: int, clock_hz: float, issue: float) -> float:
    """Give one draw's GPU time: the longer of one warp's run and the schedulers' share of every warp's issue.

    `cycles` is one warp's run (run()), `issue` the cycles a warp occupies its scheduler (issue_cycles()). The draw
    covers ceil(pixels / 32) warps spread over the SCHEDULERS schedulers; occupancy, memory and launch costs are not
    modelled.
    """
    warps = math.ceil(pixels / WARP)
    return max(cycles, math.ceil(warps / SCHEDULERS) * issue) / clock_hz * 1e9
