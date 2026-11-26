"""DMA coherency on the emulator: the guest's cache maintenance against the xHCI's reads and writes of guest RAM.

QEMU has no caches, so a missing maintenance never shows as wrong data here. Patch 0056 logs instead (cache.bin, with
`--cache-log`), in TCG's order: each data-cache maintenance instruction with the line it resolved to through the
guest's own translation at that moment; each range the controller read (an OUT transfer's data) or wrote (an IN
transfer's); and each mapping the CPU installed for a page the controller touched. The TLBs are flushed after every
transfer, so the CPU's next access to the page shows as a mapping with its MAIR attribute. Per 64-byte line, on a
non-coherent Tegra X1:

- written by the controller: when the CPU next touches the page through a cacheable mapping, an invalidate (DC IVAC or
  CIVAC, or set/way) of the line must have come after the write; no clean-only (DC CVAC) may come before it, which
  would write a stale cached copy over the data. An access through a non-cacheable mapping, or none, needs nothing;
- read by the controller: after the CPU's last cacheable access to the page since the controller last touched the
  line, a clean (DC CVAC or CIVAC, or set/way) of the line. A page's first transfer is not judged for a read: its
  earlier accesses came before the page was watched.

Only transfer data is logged: rings and contexts are not judged. A mapping is judged per page, not per line.
"""

import bisect
import struct
from collections import Counter, defaultdict
from collections.abc import Iterator
from dataclasses import dataclass, field
from pathlib import Path

RECORD = struct.Struct('<BBBBIQQQQQ')
LINE = 64
NO_ADDRESS = (1 << 64) - 1
OPS = {0x61: 'IVAC', 0xA1: 'CVAC', 0xB1: 'CVAU', 0xE1: 'CIVAC', 0x62: 'ISW', 0xA2: 'CSW', 0xE2: 'CISW'}
INVALIDATES = {'IVAC', 'CIVAC'}
CLEANS = {'CVAC', 'CIVAC'}


@dataclass(frozen=True)
class Record:
    kind: str  # D maintenance by VA, S by set/way, R controller read, W controller write, M a CPU mapping
    op: str  # the maintenance, or the MMU index of a mapping
    who: int  # vCPU, or slot for R/W
    attrs: int  # MAIR byte, or endpoint for R/W
    length: int
    seq: int
    ns: int  # guest time of a transfer; the PC of the block that made a mapping (M)
    ram: int  # ram_addr; the link register of a mapping (M)
    addr: int  # D: the resolved physical line; R/W: the IOVA; M: the physical page
    va: int  # D and M: the virtual address; R/W: the guest physical address


@dataclass
class Violation:
    rule: str
    transfer: Record
    line: int
    ops: list[Record] = field(default_factory=list)


def scan(path: Path) -> Iterator[tuple]:
    with path.open('rb') as f:
        while chunk := f.read(RECORD.size * 65536):
            yield from RECORD.iter_unpack(chunk[: len(chunk) // RECORD.size * RECORD.size])


def records(path: Path) -> list[Record]:
    """Return the records a judgement needs.

    Every transfer, set/way operation and mapping, and the maintenance by VA on the lines the controller touched: a
    boot logs millions of others, so the file is read twice and never held.
    """
    if not path.exists():
        return []
    watched, kept = set(), []
    for fields in scan(path):
        if chr(fields[0]) in 'RWSM':
            record = Record(chr(fields[0]), OPS.get(fields[1], str(fields[1])), *fields[2:])
            kept.append(record)
            if record.kind in 'RW' and record.va != NO_ADDRESS:
                watched.update(lines_of(record))
    kept.extend(
        Record('D', OPS.get(fields[1], ''), *fields[2:])
        for fields in scan(path)
        if chr(fields[0]) == 'D' and fields[8] != NO_ADDRESS and fields[8] // LINE in watched
    )
    return sorted(kept, key=lambda r: r.seq)


def lines_of(record: Record) -> range:
    """Return a transfer's guest physical lines."""
    return range(record.va // LINE, (record.va + max(record.length, 1) - 1) // LINE + 1)


def cacheable(mapping: Record) -> bool:
    """Return whether a MAIR attribute byte is Normal memory with a cacheable inner or outer policy."""
    return mapping.attrs >> 4 not in (0x0, 0x4) or mapping.attrs & 0xF not in (0x0, 0x4)


def judge(log: list[Record]) -> list[Violation]:
    """Return every violation of the rules above, in the order the log reached it."""
    set_way = [r.seq for r in log if r.kind == 'S' and r.op in ('ISW', 'CISW', 'CSW')]
    by_line: dict[int, list[Record]] = defaultdict(list)
    mappings: dict[int, list[Record]] = defaultdict(list)
    for r in log:
        if r.kind == 'M':
            mappings[r.addr >> 12].append(r)
        elif r.kind == 'D':
            by_line[r.addr // LINE].append(r)
        elif r.kind in 'RW' and r.va != NO_ADDRESS:
            for line in lines_of(r):
                by_line[line].append(r)
    violations = []
    for line, events in by_line.items():
        page = mappings.get(line * LINE >> 12, [])
        transfers = [e for e in events if e.kind in 'RW']
        for previous, event in zip([None, *transfers], [*transfers, None], strict=True):
            start = previous.seq if previous else -1
            end = event.seq if event else None
            ops = [e for e in events if e.kind == 'D' and start < e.seq and (end is None or e.seq < end)]
            touched = [m for m in page if start < m.seq and (end is None or m.seq < end)]
            if previous is not None and previous.kind == 'W':
                violations += after_write(previous, line, ops, touched, set_way)
            if event is not None and event.kind == 'R' and previous is not None:
                violations += before_read(event, line, ops, touched, set_way)
    return sorted(violations, key=lambda v: (v.transfer.seq, v.line))


def first_done(ops: list[Record], wanted: set[str], set_way: list[int], start: int, end: int | None) -> int | None:
    """Return the seq of the first op in `wanted` (or set/way) after `start` and before `end`, None if none."""
    by_va = next((op.seq for op in ops if op.op in wanted and op.seq > start), None)
    i = bisect.bisect_right(set_way, start)
    sweep = set_way[i] if i < len(set_way) else None
    found = min((s for s in (by_va, sweep) if s is not None), default=None)
    return found if found is not None and (end is None or found < end) else None


def after_write(
    write: Record, line: int, ops: list[Record], touched: list[Record], set_way: list[int]
) -> list[Violation]:
    use = next((m for m in touched if cacheable(m)), None)
    if use is None:
        return []
    found = []
    invalidated = first_done(ops, INVALIDATES, set_way, write.seq, use.seq)
    if any(op.op == 'CVAC' and op.seq < (invalidated or use.seq) for op in ops):
        found.append(Violation('clean-only of a line the controller wrote, before any invalidate', write, line, ops))
    if invalidated is None:
        rule = 'the CPU touched it through a cacheable mapping with no invalidate after the controller wrote it'
        found.append(Violation(rule, write, line, [*ops, use]))
    return found


def before_read(
    read: Record, line: int, ops: list[Record], touched: list[Record], set_way: list[int]
) -> list[Violation]:
    last = next((m for m in reversed(touched) if cacheable(m)), None)
    if last is None or first_done(ops, CLEANS, set_way, last.seq, read.seq) is not None:
        return []
    rule = 'read with no clean after the CPU touched it through a cacheable mapping'
    return [Violation(rule, read, line, [*ops, last])]


def describe(t: Record) -> str:
    direction = 'controller wrote' if t.kind == 'W' else 'controller read'
    return (
        f'{direction} PA {t.va:#x}+{t.length:#x} (IOVA {t.addr:#x}) slot {t.who} endpoint {t.attrs} '
        f'at {t.ns / 1e9:.6f} s (seq {t.seq})'
    )


def mapping_summary(log: list[Record]) -> str:
    seen = Counter((m.va >> 40 == 0xFFFFFF, m.attrs) for m in log if m.kind == 'M')
    return (
        ', '.join(f'{"kernel" if k else "other"} VA MAIR {a:#04x} x{n}' for (k, a), n in seen.most_common(6)) or 'none'
    )


def page_reasons(log: list[Record], violations: list[Violation]) -> list[str]:
    """Return one line per transfer page: what the CPU touched it through, and why it passed or that it failed."""
    lines = []
    failing = Counter(v.transfer.va >> 12 for v in violations)
    transfers = defaultdict(list)
    for r in log:
        if r.kind in 'RW' and r.va != NO_ADDRESS:
            transfers[r.va >> 12].append(r)
    for page, done_here in sorted(transfers.items()):
        first = done_here[0].seq
        used = Counter((m.va, m.attrs) for m in log if m.kind == 'M' and m.addr >> 12 == page and m.seq > first)
        kept = Counter(r.op for r in log if r.kind == 'D' and r.addr >> 12 == page)
        aliases = ', '.join(f'VA {va:#x} MAIR {attrs:#04x} x{n}' for (va, attrs), n in used.most_common(4)) or 'none'
        if failing[page]:
            verdict = f'FAIL ({failing[page]} lines)'
        elif not used:
            verdict = 'not exercised: the CPU never touched it after a transfer'
        elif not any(cacheable(Record('M', '', 0, attrs, 0, 0, 0, 0, 0, 0)) for _, attrs in used):
            verdict = 'pass (i): every alias the CPU used is non-cacheable'
        else:
            verdict = 'pass (ii): cacheable, maintained'
        kinds = Counter(f'{r.kind} ep {r.attrs}' for r in done_here)
        lines.append(
            f'  page {page << 12:#x}: {dict(kinds)}; CPU aliases {aliases}; maintenance {dict(kept)}; {verdict}'
        )
    return lines


def report(path: Path) -> tuple[list[str], list[str]]:
    """Return (problems, notes) for a run's cache.bin."""
    log = records(path)
    if not log:
        return [], [f'cache log: none ({path})']
    transfers = [r for r in log if r.kind in 'RW']
    maintenance = Counter(r.op for r in log if r.kind in 'DS')
    notes = [
        (
            f'cache log: {len(transfers)} controller transfers ({sum(r.kind == "W" for r in transfers)} written, '
            f'{sum(r.kind == "R" for r in transfers)} read), maintenance on their lines {dict(maintenance)}'
        ),
        f'transfer pages above 4 GiB: {sum(1 for r in transfers if r.va != NO_ADDRESS and r.va >= 1 << 32)}',
        f'CPU mappings of transfer pages: {mapping_summary(log)}',
    ]
    violations = judge(log)
    notes += page_reasons(log, violations)
    if not violations:
        return [], notes
    per_rule = Counter(v.rule for v in violations)
    pages = Counter(v.transfer.va >> 12 for v in violations)
    uses = Counter(r.va >> 12 for r in transfers)
    notes.append('violations: ' + '; '.join(f'{rule}: {n} lines' for rule, n in per_rule.items()))
    notes += [f'  page {p << 12:#x}: {n} violating lines over {uses[p]} transfers' for p, n in pages.most_common(8)]
    first = violations[0]
    notes.append(f'first: {first.rule}: line {first.line * LINE:#x}, {describe(first.transfer)}')
    notes += [
        f'  {op.kind} {op.op} VA {op.va:#x} -> PA {op.addr:#x} vCPU {op.who} MAIR {op.attrs:#04x} (seq {op.seq})'
        for op in first.ops[:8]
    ] or ['  no maintenance on the line in that window']
    return [f'{n} lines: {rule}' for rule, n in per_rule.items()], notes
