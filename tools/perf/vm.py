"""Page-ins and shared-cache unnests logged by the kernel (probes-kernel SwitchSched page-ins and SwitchUnnest)."""

from __future__ import annotations

import re
from collections import Counter
from dataclasses import dataclass, field
from datetime import datetime, timedelta

from tools.perf.source import Rec
from tools.perf.symbols import Symbols
from tools.perf.windows import Window

PAGEIN_HEAD = re.compile(
    r'^SwitchSched page-ins \((?P<ms>\d+) ms\): (?P<reads>\d+) reads, (?P<kib>\d+) KiB asked, '
    r'(?P<read_ms>\d+) ms reading; (?P<waits>\d+) waits for pages being read, (?P<wait_ms>\d+) ms.*?'
    r'(?:XNU page-ins (?P<xnu>\d+), (?P<again>\d+) evicted and paged in again[^;]*)?'
    r'; by process \(reads/KiB asked/ms reading/waits/ms waiting\):(?P<procs>.*)$'
)
PAGEIN_FILES = re.compile(r'^SwitchSched page-ins by file \((?P<ms>\d+) ms; [^)]*\):(?P<files>.*)$')
PAGEIN_ROW = re.compile(
    r' ?(?P<name>[^,]+?) (?P<reads>\d+)/(?P<kib>\d+)/(?P<read_ms>\d+)/(?P<waits>\d+)/(?P<wait_ms>\d+)(?:,|$)'
)
UNNEST = re.compile(
    r'^SwitchUnnest: (?P<proc>.+?)\[(?P<pid>-?\d+)\] unnest 0x(?P<start>[0-9a-fA-F]+)->0x(?P<end>[0-9a-fA-F]+): '
    r'call (?P<call>-?\d+) from user pc (?P<pc>.+?) lr (?P<lr>[^,;]+?)'
    r'(?:, frames(?P<frames>[^;]*))?; kernel(?P<kernel>.*)$'
)
WHERE_TOKEN = re.compile(r'dsc\s+0x[0-9a-fA-F]+|main\+0x[0-9a-fA-F]+|0x[0-9a-fA-F]+')
PAIR_GAP = timedelta(seconds=2)
TOP = 5

CALL_NAMES = {
    73: 'munmap',
    74: 'mprotect',
    197: 'mmap',
    294: 'shared_region_check_np',
    295: 'shared_region_map_np',
    436: 'shared_region_map_and_slide_np',
    538: 'shared_region_map_and_slide_2_np',
    -10: 'mach_vm_allocate',
    -12: 'mach_vm_deallocate',
    -14: 'mach_vm_protect',
    -15: 'mach_vm_map',
}


@dataclass(frozen=True)
class PageinEntry:
    name: str
    reads: int
    kib: int
    read_ms: int
    waits: int
    wait_ms: int

    @property
    def total_ms(self) -> int:
        return self.read_ms + self.wait_ms


@dataclass
class PageinWindow:
    time: datetime
    ms: int
    reads: int
    kib: int
    read_ms: int
    waits: int
    wait_ms: int
    xnu_pageins: int = 0
    xnu_refaults: int = 0
    procs: list[PageinEntry] = field(default_factory=list)
    files: list[PageinEntry] = field(default_factory=list)


@dataclass(frozen=True)
class UnnestEvent:
    time: datetime
    proc: str
    pid: int
    start: int
    end: int
    call: int
    pc: str
    lr: str
    kernel: tuple[int, ...] = ()
    frames: tuple[str, ...] = ()

    @property
    def size(self) -> int:
        return max(0, self.end - self.start)


def _parse_rows(text: str) -> list[PageinEntry]:
    return [
        PageinEntry(
            m['name'].strip(),
            int(m['reads']),
            int(m['kib']),
            int(m['read_ms']),
            int(m['waits']),
            int(m['wait_ms']),
        )
        for m in PAGEIN_ROW.finditer(text)
    ]


def pagein_windows(recs: list[Rec]) -> list[PageinWindow]:
    """Parse SwitchSched page-ins and join each with its following by-file line."""
    out: list[PageinWindow] = []
    for r in recs:
        if r.proc != 'kernel':
            continue
        if m := PAGEIN_HEAD.match(r.msg):
            out.append(
                PageinWindow(
                    time=r.time,
                    ms=int(m['ms']),
                    reads=int(m['reads']),
                    kib=int(m['kib']),
                    read_ms=int(m['read_ms']),
                    waits=int(m['waits']),
                    wait_ms=int(m['wait_ms']),
                    xnu_pageins=int(m['xnu'] or 0),
                    xnu_refaults=int(m['again'] or 0),
                    procs=_parse_rows(m['procs']),
                )
            )
        elif (fm := PAGEIN_FILES.match(r.msg)) and out and abs(r.time - out[-1].time) <= PAIR_GAP:
            out[-1].files = _parse_rows(fm['files'])
    return out


def _accumulate(entries: list[PageinEntry]) -> list[PageinEntry]:
    merged: dict[str, list[int]] = {}
    for e in entries:
        cur = merged.setdefault(e.name, [0, 0, 0, 0, 0])
        cur[0] += e.reads
        cur[1] += e.kib
        cur[2] += e.read_ms
        cur[3] += e.waits
        cur[4] += e.wait_ms
    rows = [PageinEntry(k, v[0], v[1], v[2], v[3], v[4]) for k, v in merged.items()]
    rows.sort(key=lambda r: (-r.total_ms, -r.kib, r.name))
    return rows


def pagein_summary(w: Window, ws: list[PageinWindow], focus_proc: str = '') -> list[str]:
    """Summarize SwitchSched page-ins overlapping one window, naming top processes and files."""
    chosen = [pw for pw in ws if w.holds(pw.time)]
    if not chosen:
        chosen = [pw for pw in ws if w.overlap(pw.time - timedelta(milliseconds=pw.ms), pw.time)]
    if not chosen:
        return []
    reads = sum(pw.reads for pw in chosen)
    kib = sum(pw.kib for pw in chosen)
    read_ms = sum(pw.read_ms for pw in chosen)
    waits = sum(pw.waits for pw in chosen)
    wait_ms = sum(pw.wait_ms for pw in chosen)
    refaults = sum(pw.xnu_refaults for pw in chosen)
    if reads == 0 and waits == 0:
        return []
    refault_note = f', {refaults} refaults' if refaults else ''
    out = [
        (
            f'page-ins ({len(chosen)} window{"s" if len(chosen) != 1 else ""}): {reads} reads '
            f'({kib} KiB, {read_ms} ms reading), {waits} busy-page waits ({wait_ms} ms waiting{refault_note})'
        )
    ]
    procs = _accumulate([e for pw in chosen for e in pw.procs])
    if procs:
        shown = procs[:TOP]
        if focus_proc and not any(p.name.startswith(focus_proc) for p in shown):
            match = next((p for p in procs if p.name.startswith(focus_proc)), None)
            if match:
                shown = [*shown[: TOP - 1], match]
        out.append('  by process: ' + ', '.join(f'{p.name} {p.kib} KiB/{p.total_ms} ms' for p in shown))
    files = _accumulate([e for pw in chosen for e in pw.files])
    if files:
        out.append('  by file: ' + ', '.join(f'{f.name} {f.kib} KiB/{f.total_ms} ms' for f in files[:TOP]))
    return out


def _parse_where_frames(text: str | None) -> tuple[str, ...]:
    if not text:
        return ()
    return tuple(re.sub(r'\s+', ' ', m.group(0)) for m in WHERE_TOKEN.finditer(text))


def unnest_lines(recs: list[Rec]) -> list[UnnestEvent]:
    """Parse SwitchUnnest log lines emitted when a process unnests part of the dyld shared cache."""
    out: list[UnnestEvent] = []
    for r in recs:
        if r.proc != 'kernel':
            continue
        if m := UNNEST.match(r.msg):
            kernel_frames = tuple(int(tok, 16) for tok in m['kernel'].split() if tok.startswith('0x'))
            out.append(
                UnnestEvent(
                    time=r.time,
                    proc=m['proc'],
                    pid=int(m['pid']),
                    start=int(m['start'], 16),
                    end=int(m['end'], 16),
                    call=int(m['call']),
                    pc=m['pc'].strip(),
                    lr=m['lr'].strip(),
                    kernel=kernel_frames,
                    frames=_parse_where_frames(m['frames']),
                )
            )
    return out


def call_label(num: int) -> str:
    name = CALL_NAMES.get(num)
    return f'{name} ({num})' if name else f'call {num}'


def _fmt_bytes(n: int) -> str:
    if n >= 1 << 20:
        return f'{n / (1 << 20):.1f} MiB'
    return f'{n / 1024:.0f} KiB'


def unnest_summary(events: list[UnnestEvent], syms: Symbols | None = None, limit: int = 5) -> list[str]:
    """Summarize SwitchUnnest events for a window or boot, symbolizing user and kernel sites when available."""
    if not events:
        return []
    total_size = sum(e.size for e in events)
    by_proc = Counter(e.proc for e in events)
    by_call = Counter(call_label(e.call) for e in events)
    out = [
        f'shared-cache unnests: {len(events)} ({_fmt_bytes(total_size)}); by process: '
        + ', '.join(f'{p} {n}' for p, n in by_proc.most_common(limit))
        + '; calls: '
        + ', '.join(f'{c} {n}' for c, n in by_call.most_common(4))
    ]
    sites: Counter[str] = Counter()
    for e in events:
        pc = syms.resolve_where(e.pc, e.proc) if syms else e.pc
        lr = syms.resolve_where(e.lr, e.proc) if syms else e.lr
        site = f'{e.proc} {call_label(e.call)} pc {pc} lr {lr}'
        if e.frames:
            chain = ' <- '.join(syms.resolve_where(f, e.proc) if syms else f for f in e.frames)
            site = f'{site} <- {chain}'
        sites[site] += 1
    out += [f'  {site} x{n}' for site, n in sites.most_common(limit)]
    return out
