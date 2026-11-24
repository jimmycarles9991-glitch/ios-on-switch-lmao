"""One boot's parsed records, and the dossier of everything logged inside one window of it."""

from __future__ import annotations

import bisect
from collections import Counter
from dataclasses import dataclass, field
from datetime import datetime, timedelta

from tools.perf import cpu, pacing, symbols, vm, windows, work
from tools.perf.source import Rec
from tools.perf.windows import Window

FAMILIES = (
    'SwitchFrame: ',
    'SwitchFrameRender',
    'SwitchFrameUser',
    'SwitchDisplay stats',
    'SwitchSched 1/2',
    'SwitchSched cores',
    'SwitchSched tasks',
    'SwitchSched page-ins',
    'SwitchPower: ',
    'SwitchKprof',
    'SwitchStall',
    'SwitchUnnest',
    'SwitchMark',
    'tegra-gm20b: stats',
    'HWTEST ',
)


@dataclass
class Session:
    boot: str
    recs: list[Rec]
    windows: dict[str, list[Window]] = field(default_factory=dict)
    stats: list[pacing.Stats] = field(default_factory=list)
    slows: list[pacing.Slow] = field(default_factory=list)  # in time order
    busy: list[cpu.Busy] = field(default_factory=list)
    tasks: list = field(default_factory=list)
    kprof: list[cpu.KprofWindow] = field(default_factory=list)
    pageins: list[vm.PageinWindow] = field(default_factory=list)
    unnests: list[vm.UnnestEvent] = field(default_factory=list)
    costs: list[work.Cost] = field(default_factory=list)  # in the order they ended
    slow_times: list[datetime] = field(default_factory=list)
    cost_ends: list[datetime] = field(default_factory=list)
    longest_cost: timedelta = timedelta(0)
    stalls: list[Rec] = field(default_factory=list)


def build(boot: str, recs: list[Rec]) -> Session:
    s = Session(boot, recs)
    s.windows = {
        'mark': windows.marks(recs),
        'launch': windows.launches(recs),
        'touch': windows.touches(recs),
        'hang': windows.hangs(recs),
        'test': windows.tests(recs),
    }
    s.stats = pacing.stats_windows(recs)
    s.slows = sorted(pacing.slow_presents(recs), key=lambda x: x.time)
    s.slow_times = [x.time for x in s.slows]
    s.busy = cpu.busy_windows(recs)
    s.tasks = cpu.tasks(recs)
    s.kprof = cpu.kprof_windows(recs)
    s.pageins = vm.pagein_windows(recs)
    s.unnests = vm.unnest_lines(recs)
    s.costs = sorted(work.compiles(recs) + work.stalls(recs), key=lambda c: c.time)
    s.cost_ends = [c.time for c in s.costs]
    s.longest_cost = timedelta(milliseconds=max((c.ms for c in s.costs), default=0.0))
    s.stalls = [r for r in recs if r.proc == 'kernel' and r.msg.startswith('SwitchStall')]
    return s


def slows_in(s: Session, w: Window) -> list[pacing.Slow]:
    return s.slows[bisect.bisect_left(s.slow_times, w.start) : bisect.bisect_right(s.slow_times, w.end)]


def costs_in(s: Session, w: Window) -> list[work.Cost]:
    """Return the costs that overlap the window: each ends inside it, or after it by at most the longest cost."""
    lo = bisect.bisect_left(s.cost_ends, w.start)
    hi = bisect.bisect_right(s.cost_ends, w.end + s.longest_cost)
    return [c for c in s.costs[lo:hi] if w.overlap(c.start, c.time) or w.holds(c.time)]


def families(s: Session) -> Counter[str]:
    counts: Counter[str] = Counter()
    for r in s.recs:
        family = next((f for f in FAMILIES if r.msg.startswith(f)), None)
        if family:
            counts[family.strip(': ')] += 1
        elif r.name:
            counts[f'signpost {r.name}'] += 1
    return counts


def lost(slows: list[pacing.Slow]) -> list[pacing.Slow]:
    return [x for x in slows if pacing.counted(x)]


def pacing_row(s: Session, w: Window) -> str:
    hist, covered, worst, around = pacing.distribution(w, s.stats)
    inside = slows_in(s, w)
    slows = lost(inside)
    untold = Counter(x.stage for x in inside if x.stage in pacing.UNTOLD)
    shown = sum(hist)
    stages = Counter(x.stage for x in slows)
    return (
        f'presents 1:{hist[0]} 2:{hist[1]} 3:{hist[2]} 4+:{hist[3]}'
        + (f' ({100 * hist[0] / shown:.0f}% one period apart, worst {worst:.0f} ms)' if shown else '')
        + f' in {covered / 1000:.0f} s of stats{" around it" if around and shown else ""}; {len(slows)} lost, '
        + f'{sum(x.frame.frames - 1 for x in slows)} periods'
        + (' (' + ', '.join(f'{k} {n}' for k, n in stages.most_common(4)) + ')' if stages else '')
        + ''.join(f'; {n} {stage}' for stage, n in untold.most_common())
    )


def dossier(s: Session, w: Window, syms: symbols.Symbols | None = None) -> list[str]:
    """Return what the lines say happened inside one window, and what they cannot say."""
    out = [pacing_row(s, w)]
    busy = cpu.mean_busy(w, s.busy)
    out.append(f'busy cores {busy:.2f} (the 10 s windows over it)' if busy is not None else 'busy cores: no line')
    if emc := cpu.mean_emc(w, s.busy):
        rates = sorted(emc.items(), key=lambda kv: -kv[1])
        out.append('memory clock: ' + ', '.join(f'{mhz} MHz {100 * share:.0f}%' for mhz, share in rates if share))
    top = cpu.top_tasks(w, s.tasks)
    out.append(
        'processes: ' + ', '.join(f'{n} {v:.0f} ms/s' for n, v in top)
        if top
        else 'processes: not attributable (needs probes-kernel SwitchSched tasks, a 10 s window ending inside)'
    )
    out += cpu.kprof_in_window(w, s.kprof, syms, w.proc)
    out += vm.pagein_summary(w, s.pageins, w.proc)
    out += vm.unnest_summary([u for u in s.unnests if w.holds(u.time)], syms)
    out += [row.text() for row in work.by_kind(costs_in(s, w))[:7]]
    hangs = [h for h in s.windows['hang'] if h is not w and w.overlap(h.start, h.end)]
    out += [f'hang {h.proc}[{h.pid}] {h.name} {h.note} from {h.start:%H:%M:%S.%f}'[:-3] for h in hangs[:4]]
    out += [f'stall line: {r.msg[:240]}' for r in s.stalls if w.holds(r.time)][:3]
    return out
