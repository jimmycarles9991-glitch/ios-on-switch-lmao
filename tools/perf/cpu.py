"""Who had the CPU: busy cores per 10 s, interrupts, traps, callouts, processes, pinned windows, and kprof."""

from __future__ import annotations

import re
from collections import Counter
from dataclasses import dataclass, field
from datetime import datetime, timedelta

from tools.devicerun.cpu import windows as task_windows
from tools.perf import kprof as kprof_mod, kprof_fmt, symbols
from tools.perf.kprof import (
    KprofFaults,
    KprofHeader,
    KprofImage,
    KprofIpis,
    KprofKernelFn,
    KprofKernelThread,
    KprofMasked,
    KprofProcFaults,
    KprofSpot,
    KprofTask,
    KprofUser,
    KprofWindow,
    kprof,
    kprof_windows,
)
from tools.perf.kprof_fmt import DetailOptions, kprof_in_window, kprof_summary
from tools.perf.source import Rec
from tools.perf.windows import Window

__all__ = [
    'Busy',
    'CalloutEntry',
    'CalloutWindow',
    'CpuWindows',
    'IrqWindow',
    'KprofFaults',
    'KprofHeader',
    'KprofImage',
    'KprofIpis',
    'KprofKernelFn',
    'KprofKernelThread',
    'KprofMasked',
    'KprofProcFaults',
    'KprofSpot',
    'KprofTask',
    'KprofUser',
    'KprofWindow',
    'SystimeWindow',
    'Waits',
    'busy_windows',
    'callout_summary',
    'callout_windows',
    'core_waits',
    'cpu_floor',
    'interrupt_summary',
    'irq_windows',
    'kprof',
    'kprof_in_window',
    'kprof_summary',
    'kprof_windows',
    'mean_busy',
    'mean_emc',
    'pinned_summary',
    'systime_summary',
    'systime_windows',
    'tasks',
    'top_tasks',
]

POWER = re.compile(r'^SwitchPower: (?P<ms>\d+) ms: ')
CORE_BUSY = re.compile(r'cpu\d c7 [\d.]+% \([^)]*\) wfi [\d.]+% busy (?P<busy>[\d.]+)%')
CORE_IRQ = re.compile(r'cpu\d+ (?P<timer>\d+) timer (?P<ipi>\d+) ipi (?P<spi>\d+) spi')
EMC = re.compile(r'EMC \d+ moves (?P<rates>(?:\d+:[\d.]+% ?)+)')
EMC_RATE = re.compile(r'(?P<mhz>\d+):(?P<share>[\d.]+)%')
IRQ_HEAD = re.compile(r'^SwitchPower interrupts: (?P<ms>\d+) ms: (?P<total>\d+) SPIs from \d+ INTIDs')
IRQ_SRC = re.compile(r'(?P<intid>\d+) \(spi \d+\) (?P<count>\d+) \(')
SYSTIME_HEAD = re.compile(r'^SwitchSched system \((?P<ms>\d+) ms[^)]*\): (?P<body>.*)$')
SYSTIME_ITEM = re.compile(r'^(?P<name>.+?) (?P<ms>\d+) ms(?: \((?P<entries>\d+)\))?$')
CALLOUT_HEAD = re.compile(r'^SwitchSched callouts \((?P<ms>\d+) ms.*?\): (?P<body>.*)$')
CALLOUT_UNLISTED = re.compile(r'^unlisted (?P<ms>\d+) ms \((?P<calls>\d+)\)$')
CALLOUT_GROUP = re.compile(r'^(?P<group>.+?)\s+(?=0x[0-9a-fA-F]+)(?P<rest>.*)$')
CALLOUT_ITEM = re.compile(r'0x(?P<addr>[0-9a-fA-F]+)\s+(?P<ms>\d+) ms \((?P<calls>\d+)\)')
CALLOUT_OTHERS = re.compile(r'\(others (?P<ms>\d+) ms\)$')
THREAD = re.compile(
    r'(?P<name>[\w.\-]+)\[(?P<pid>\d+)\] [^:;]*: (?P<runs>\d+) runs (?P<p50>\d+)/(?P<p99>\d+)/(?P<max>\d+) us'
    r'(?:, longest began (?P<before>\d+) ms before the window)?'
)
PINNED_CORES = 3.5

TEGRA_IRQ_NAMES: dict[int, str] = {
    46: 'SDMMC1',
    69: 'UART-B JoyCon-R',
    70: 'I2C1',
    76: 'XUDC USB',
    78: 'UART-C JoyCon-L',
    90: 'TegraSE1',
    97: 'Host1x syncpt',
    105: 'Display-A',
    116: 'I2C2 ALS',
    119: 'GPIO Bank5',
    122: 'UART-D BT',
    124: 'I2C3 Touch',
    125: 'SPI4 IMU',
    131: 'PCIe WLAN MSI',
    139: 'APBDMA-ch3 BT-RX',
    190: 'GM20B GPU',
}


@dataclass
class Busy:
    end: datetime
    ms: int
    cores: float
    emc: dict[int, float]  # the memory clock's residency in the window: MHz: share
    timer_irqs: int = 0
    ipis: int = 0
    spis: int = 0


@dataclass
class IrqWindow:
    time: datetime
    ms: int
    total: int
    counts: dict[int, int] = field(default_factory=dict)


@dataclass
class SystimeWindow:
    time: datetime
    ms: int
    buckets: dict[str, tuple[int, int]] = field(default_factory=dict)


@dataclass(frozen=True)
class CalloutEntry:
    group: str
    addr: int
    ms: int
    calls: int


@dataclass
class CalloutWindow:
    time: datetime
    ms: int
    entries: list[CalloutEntry] = field(default_factory=list)
    others_ms: int = 0
    unlisted_ms: int = 0
    unlisted_calls: int = 0


@dataclass
class Waits:
    runs: int = 0
    worst_p99: int = 0
    worst_us: int = 0
    when: datetime | None = None  # the end of the 10 s window the longest wait ended in
    began_before_ms: int = 0  # how long before that window it began
    booting: bool = False  # that window ended before System Boot Complete


@dataclass(frozen=True)
class CpuWindows:
    busy: list[Busy]
    tasks: list
    kprof: list[KprofWindow] = field(default_factory=list)
    irqs: list[IrqWindow] = field(default_factory=list)
    systime: list[SystimeWindow] = field(default_factory=list)
    callouts: list[CalloutWindow] = field(default_factory=list)


def busy_windows(recs: list[Rec]) -> list[Busy]:
    out = []
    for r in recs:
        if r.proc == 'kernel' and (m := POWER.match(r.msg)):
            cores = sum(float(c['busy']) for c in CORE_BUSY.finditer(r.msg)) / 100
            rates = EMC.search(r.msg)
            emc = {int(e['mhz']): float(e['share']) / 100 for e in EMC_RATE.finditer(rates['rates'])} if rates else {}
            timers = ipis = spis = 0
            for ci in CORE_IRQ.finditer(r.msg):
                timers += int(ci['timer'])
                ipis += int(ci['ipi'])
                spis += int(ci['spi'])
            out.append(Busy(r.time, int(m['ms']), cores, emc, timers, ipis, spis))
    return out


def irq_windows(recs: list[Rec]) -> list[IrqWindow]:
    out = []
    for r in recs:
        if r.proc == 'kernel' and (m := IRQ_HEAD.match(r.msg)) and int(m['ms']):
            counts = {int(s['intid']): int(s['count']) for s in IRQ_SRC.finditer(r.msg)}
            out.append(IrqWindow(r.time, int(m['ms']), int(m['total']), counts))
    return out


def systime_windows(recs: list[Rec]) -> list[SystimeWindow]:
    out = []
    for r in recs:
        if r.proc == 'kernel' and (m := SYSTIME_HEAD.match(r.msg)) and int(m['ms']):
            buckets = {
                im['name']: (int(im['ms']), int(im['entries'] or 0))
                for seg in m['body'].split(',')
                if (im := SYSTIME_ITEM.match(seg.strip()))
            }
            if buckets:
                out.append(SystimeWindow(r.time, int(m['ms']), buckets))
    return out


def callout_windows(recs: list[Rec]) -> list[CalloutWindow]:
    out = []
    for r in recs:
        if r.proc != 'kernel' or not (m := CALLOUT_HEAD.match(r.msg)) or not int(m['ms']):
            continue
        cw = CalloutWindow(r.time, int(m['ms']))
        for seg in m['body'].split(';'):
            part = seg.strip()
            if not part:
                continue
            if um := CALLOUT_UNLISTED.match(part):
                cw.unlisted_ms, cw.unlisted_calls = int(um['ms']), int(um['calls'])
            elif gm := CALLOUT_GROUP.match(part):
                group = gm['group'].strip()
                cw.entries += [
                    CalloutEntry(group, int(im['addr'], 16), int(im['ms']), int(im['calls']))
                    for im in CALLOUT_ITEM.finditer(gm['rest'])
                ]
                if om := CALLOUT_OTHERS.search(gm['rest']):
                    cw.others_ms += int(om['ms'])
        if cw.entries or cw.others_ms or cw.unlisted_ms:
            out.append(cw)
    return out


def mean_busy(window: Window, busy: list[Busy]) -> float | None:
    """Return the busy cores of the 10 s windows over the window, weighted by how much of each lies inside it."""
    weights = [(b, window.overlap(b.end - timedelta(milliseconds=b.ms), b.end)) for b in busy]
    total = sum(w for _, w in weights)
    return sum(b.cores * w for b, w in weights) / total if total else None


def mean_emc(window: Window, busy: list[Busy]) -> dict[int, float]:
    """Return the memory clock's residency over the window (MHz: share), weighted like mean_busy."""
    shares: dict[int, float] = {}
    total = 0.0
    for b in busy:
        w = window.overlap(b.end - timedelta(milliseconds=b.ms), b.end)
        total += w if b.emc else 0
        for mhz, share in b.emc.items():
            shares[mhz] = shares.get(mhz, 0.0) + share * w
    return {mhz: s / total for mhz, s in shares.items()} if total else {}


def tasks(recs: list[Rec]) -> list:
    """Return probes-kernel's windows (tools/devicerun/cpu.py), each with its processes' user + system ms."""

    def fields(r: Rec) -> dict:
        return {'message': r.msg, 'time': r.time, 'boot': r.boot}

    cores = [fields(r) for r in recs if r.proc == 'kernel' and r.msg.startswith('SwitchSched cores (')]
    lines = [fields(r) for r in recs if r.proc == 'kernel' and r.msg.startswith('SwitchSched tasks (')]
    return task_windows(cores, lines)


def top_tasks(window: Window, ws: list, n: int = 8) -> list[tuple[str, float]]:
    """Return the processes by user + system ms per second over the task windows ending inside the window."""
    used: Counter[str] = Counter()
    seconds = 0.0
    for w in ws:
        if window.holds(w.time) and w.tasks:
            used.update(w.tasks)
            seconds += w.ms / 1000
    return [(name, total / seconds) for name, total in used.most_common(n)] if seconds else []


def core_waits(recs: list[Rec]) -> dict[str, Waits]:
    """Return, per named thread, its runs and its worst runnable-to-running p99 and maximum over the session."""
    out: dict[str, Waits] = {}
    for r in recs:
        if r.proc != 'kernel' or not r.msg.startswith(('SwitchSched 1/2', 'SwitchSched 2/2')):
            continue
        for m in THREAD.finditer(r.msg):
            w = out.setdefault(m['name'], Waits())
            w.runs += int(m['runs'])
            w.worst_p99 = max(w.worst_p99, int(m['p99']))
            if int(m['max']) > w.worst_us:
                w.worst_us, w.when, w.booting = int(m['max']), r.time, 'no System Boot Complete yet' in r.msg
                w.began_before_ms = int(m['before'] or 0)
    return out


def _irq_label(intid: int) -> str:
    name = TEGRA_IRQ_NAMES.get(intid)
    return f'INTID {intid} ({name})' if name else f'INTID {intid} (SPI {intid - 32})'


def interrupt_summary(irqs: list[IrqWindow], busy: list[Busy], prefix: str = '') -> list[str]:
    out = []
    with_irqs = [b for b in busy if b.timer_irqs or b.ipis or b.spis]
    if with_irqs and (sec := sum(b.ms for b in with_irqs) / 1000):
        t = sum(b.timer_irqs for b in with_irqs) / sec
        i = sum(b.ipis for b in with_irqs) / sec
        sp = sum(b.spis for b in with_irqs) / sec
        out.append(f'{prefix}interrupts ({len(with_irqs)} windows): {t:.0f} timer/s, {i:.0f} ipi/s, {sp:.0f} spi/s')
    if irqs and (sec := sum(w.ms for w in irqs) / 1000):
        counts: Counter[int] = Counter()
        for w in irqs:
            counts.update(w.counts)
        total = sum(w.total for w in irqs) / sec
        top = ', '.join(f'{_irq_label(intid)} {n / sec:.0f}/s' for intid, n in counts.most_common(8))
        line = (
            f'{prefix}SPI sources ({total:.0f}/s over {len(irqs)} windows): {top}'
            if top
            else f'{prefix}SPI sources: 0/s'
        )
        out.append(line)
    return out


def systime_summary(ws: list[SystimeWindow], prefix: str = '') -> list[str]:
    if not ws or not (sec := sum(w.ms for w in ws) / 1000):
        return []
    ms_sum: Counter[str] = Counter()
    ent_sum: Counter[str] = Counter()
    for w in ws:
        for name, (ms, ent) in w.buckets.items():
            ms_sum[name] += ms
            ent_sum[name] += ent
    total_ms = sum(ms_sum.values())
    parts = [
        f'{name} {ms / sec:.0f} ms/s' + (f' ({ent_sum[name] / sec:.0f}/s)' if ent_sum[name] else '')
        for name, ms in ms_sum.most_common()
        if ms or ent_sum[name]
    ]
    if not parts:
        return []
    return [f'{prefix}SwitchSched system ({total_ms / sec:.0f} ms/s over {len(ws)} windows): ' + ', '.join(parts)]


def callout_summary(
    ws: list[CalloutWindow],
    syms: symbols.Symbols | None = None,
    prefix: str = '',
    limit: int = 8,
) -> list[str]:
    if not ws or not (sec := sum(w.ms for w in ws) / 1000):
        return []
    by_fn: Counter[tuple[str, int]] = Counter()
    calls_by_fn: Counter[tuple[str, int]] = Counter()
    others = unlisted_ms = unlisted_calls = 0
    for w in ws:
        for e in w.entries:
            by_fn[(e.group, e.addr)] += e.ms
            calls_by_fn[(e.group, e.addr)] += e.calls
        others += w.others_ms
        unlisted_ms += w.unlisted_ms
        unlisted_calls += w.unlisted_calls
    total_ms = sum(by_fn.values()) + others + unlisted_ms
    parts = [
        f'{group} {kprof_mod.fmt_kaddr("", addr, syms)} {ms / sec:.0f} ms/s ({calls_by_fn[(group, addr)] / sec:.0f}/s)'
        for (group, addr), ms in by_fn.most_common(limit)
    ]
    if unlisted_ms:
        parts.append(f'unlisted {unlisted_ms / sec:.0f} ms/s ({unlisted_calls / sec:.0f}/s)')
    if not parts:
        return []
    return [f'{prefix}SwitchSched callouts ({total_ms / sec:.0f} ms/s over {len(ws)} windows): ' + ', '.join(parts)]


def _overlaps_any(t_end: datetime, ms: int, spans: list[Window]) -> bool:
    t_start = t_end - timedelta(milliseconds=ms)
    # Require half-window coverage so a sibling summary logged a few ms past a boundary never bleeds in.
    return sum(sp.overlap(t_start, t_end) for sp in spans) * 2 >= ms


def _pinned_spans(
    busy: list[Busy], sched_tasks: list, threshold: float
) -> tuple[list[Window], list[Busy], int, float, float]:
    if busy:
        picked = [b for b in busy if b.cores >= threshold]
        spans = [Window('pinned', 'pinned', b.end - timedelta(milliseconds=b.ms), b.end) for b in picked]
        mean = sum(b.cores for b in picked) / len(picked) if picked else 0.0
        return spans, picked, len(busy), mean, sum(b.ms for b in picked) / 1000
    picked_tw = [tw for tw in sched_tasks if tw.busy >= threshold]
    spans = [Window('pinned', 'pinned', tw.time - timedelta(milliseconds=tw.ms), tw.time) for tw in picked_tw]
    mean = sum(tw.busy for tw in picked_tw) / len(picked_tw) if picked_tw else 0.0
    return spans, [], len(sched_tasks), mean, sum(tw.ms for tw in picked_tw) / 1000


def _sched_proc_line(sched_tasks: list, spans: list[Window], label: str, limit: int) -> str:
    used: Counter[str] = Counter()
    sec = 0.0
    for tw in sched_tasks:
        if tw.tasks and _overlaps_any(tw.time, tw.ms, spans):
            used.update(tw.tasks)
            sec += tw.ms / 1000
    if not used or not sec:
        return ''
    return f'  {label} (SwitchSched tasks): ' + ', '.join(f'{n} {v / sec:.0f} ms/s' for n, v in used.most_common(limit))


def _kprof_proc_line(ws: list[KprofWindow], label: str, limit: int) -> str:
    sec = sum(kw.ms for kw in ws) / 1000
    if not sec:
        return ''
    kp_ms: Counter[str] = Counter()
    for kw in ws:
        for t in kw.tasks:
            kp_ms[t.name] += t.cpu_ms
    if not kp_ms:
        return ''
    return f'  {label} (SwitchKprof): ' + ', '.join(f'{n} {v / sec:.0f} ms/s' for n, v in kp_ms.most_common(limit))


def pinned_summary(
    data: CpuWindows,
    syms: symbols.Symbols | None = None,
    threshold: float = PINNED_CORES,
) -> list[str]:
    """Aggregate processes, traps, callouts, SPIs, and kprof over pinned windows (>= threshold cores)."""
    spans, picked_busy, total_w, mean_cores, total_s = _pinned_spans(data.busy, data.tasks, threshold)
    if not spans:
        return []
    head = (
        f'Pinned CPU (>= {threshold:.1f} busy cores): {len(spans)} of {total_w} windows ({total_s:.0f} s), '
        f'mean {mean_cores:.2f} busy cores'
    )
    with_irqs = [b for b in picked_busy if b.timer_irqs or b.ipis or b.spis]
    if with_irqs and (irq_s := sum(b.ms for b in with_irqs) / 1000):
        head += (
            f'; {sum(b.timer_irqs for b in with_irqs) / irq_s:.0f} timer/s, '
            f'{sum(b.ipis for b in with_irqs) / irq_s:.0f} ipi/s, '
            f'{sum(b.spis for b in with_irqs) / irq_s:.0f} spi/s'
        )
    out = [head]
    if line := _sched_proc_line(data.tasks, spans, 'pinned processes', 10):
        out.append(line)
    pin_kp = [kw for kw in data.kprof if _overlaps_any(kw.time, kw.ms, spans)]
    if line := _kprof_proc_line(pin_kp, 'pinned processes', 10):
        out.append(line)
    out += interrupt_summary([w for w in data.irqs if _overlaps_any(w.time, w.ms, spans)], [], prefix='  pinned ')
    out += systime_summary([w for w in data.systime if _overlaps_any(w.time, w.ms, spans)], prefix='  pinned ')
    out += callout_summary([w for w in data.callouts if _overlaps_any(w.time, w.ms, spans)], syms, prefix='  pinned ')
    if pin_kp:
        opts = DetailOptions(prefix='  pinned ', fn_limit=8, kt_limit=6, masked_limit=5, user_limit=4, img_limit=3)
        out += kprof_fmt.detail_lines(pin_kp, syms, opts)
    return out


def _floor_spans(busy: list[Busy], sched_tasks: list, kprof_ws: list[KprofWindow]) -> tuple[list[Window], float | None]:
    if busy:
        picked = sorted(busy, key=lambda b: b.cores)[: max(1, len(busy) // 4)]
        spans = [Window('floor', 'floor', b.end - timedelta(milliseconds=b.ms), b.end) for b in picked]
        return spans, sum(b.cores for b in picked) / len(picked)
    if sched_tasks:
        picked = sorted(sched_tasks, key=lambda tw: tw.busy)[: max(1, len(sched_tasks) // 4)]
        spans = [Window('floor', 'floor', tw.time - timedelta(milliseconds=tw.ms), tw.time) for tw in picked]
        return spans, sum(tw.busy for tw in picked) / len(picked)
    if kprof_ws:
        picked = sorted(kprof_ws, key=lambda kw: sum(t.cpu_ms for t in kw.tasks))[: max(1, len(kprof_ws) // 4)]
        spans = [Window('floor', 'floor', kw.time - timedelta(milliseconds=kw.ms), kw.time) for kw in picked]
        return spans, None
    return [], None


def cpu_floor(
    busy: list[Busy],
    sched_tasks: list,
    kprof_ws: list[KprofWindow],
    syms: symbols.Symbols | None = None,
) -> list[str]:
    """Attribute the background CPU floor (quietest quarter of 10 s windows) via SwitchSched tasks and SwitchKprof."""
    spans, mean_cores = _floor_spans(busy, sched_tasks, kprof_ws)
    if not spans:
        return []
    head = (
        f'CPU floor (quietest quarter, {len(spans)} windows): mean {mean_cores:.2f} busy cores'
        if mean_cores is not None
        else f'CPU floor (quietest quarter, {len(spans)} kprof windows)'
    )
    out = [head]
    if line := _sched_proc_line(sched_tasks, spans, 'floor processes', 8):
        out.append(line)
    floor_kp = [kw for kw in kprof_ws if _overlaps_any(kw.time, kw.ms, spans)]
    if floor_kp:
        if line := _kprof_proc_line(floor_kp, 'floor processes', 8):
            out.append(line)
        out += ['  ' + line for line in kprof_fmt.hot_lines(floor_kp, syms, '', limit=6)]
        out += ['  ' + line for line in kprof_fmt.image_lines(floor_kp, syms, '', limit=4)]
    return out
