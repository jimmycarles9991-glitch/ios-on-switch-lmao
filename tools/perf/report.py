"""The session report: one text, one section per question, every window's dossier, and what the lines cannot say."""

from __future__ import annotations

import os
from collections import Counter
from dataclasses import dataclass, field
from datetime import datetime

from tools.perf import cpu, dumps, gpu, ips, ktrace, pacing, symbols, vm, work
from tools.perf.session import Session, dossier, families, lost, pacing_row, slows_in
from tools.perf.windows import BOOTSTRAPPED, Window

WATCH = ('SpringBoard', 'backboardd')
FRAME_THREADS = (
    'render-server',
    'display.primary',
    'raw.submit',
    'raw.completion',
    'SpringBoard.main',
    'backboardd.main',
)
TOP_TESTS = 20
TIMELINES = 12
WAITED_TIMELINES = 3
HANG_DOSSIERS = 12
HANG_TESTS = 6
DUMP_SLACK_S = 3600  # a dump's file is written at its trigger; a pull can come an hour later
SLOW_WAIT_US = 10000
FOCUS = ('pacing.', 'ui.', 'display.snapshot')
LIMITS = (
    (
        'SwitchFrame is written only for presents two periods or more after the one before and under 250 ms apart '
        '(gaps inside a touch window from probes-frame 4 on); a still screen writes no stats line.'
    ),
    (
        'An unanchored slow present had no frame end in the two periods before it (the display was idle): content '
        'cadence or a late frame, which these lines cannot tell apart (probes-frame 3 names them masked).'
    ),
    (
        'A gap before it: the present kept its own chain, but the frame before it never presented. It came late and '
        'was replaced, its content came late, or there was none; only the commit probe tells these apart.'
    ),
    (
        'SwitchFrameRender is capped at 30/s and SwitchFrameUser at 20/s a process (60/s with probes-frame 11): the '
        '"dropped" counts on those lines say how many a window lost.'
    ),
    (
        'Without --//kernel:recorder or --//kernel:kprof, no line names the user functions a thread ran over a window; '
        'pass --dsc (or extract tables under ~/Library/Application Support/switch-ios/symbols/<build>/) to symbolize.'
    ),
    (
        'HangTracer measures every main-thread hang but writes no report with stacks on this device, and tailspin is '
        'off: a hang has stacks only when the flight recorder dumped it.'
    ),
)


@dataclass
class Pulled:
    """What the pull holds besides the log: crash reports, image paths the log named, recorder dumps, symbols."""

    reports: list[ips.Report] = field(default_factory=list)
    images: dict[str, str] = field(default_factory=dict)
    spins: list[str] = field(default_factory=list)
    dumps: list[tuple[str, str]] = field(default_factory=list)  # (ring, stackshot), oldest first
    kcdata: str | None = None  # XNU's tools/lldbmacros/kcdata.py
    syms: symbols.Symbols = field(default_factory=symbols.Symbols)


def at(t: datetime) -> str:
    return f'{t:%H:%M:%S.%f}'[:-3]


def whole(s: Session) -> Window:
    return Window('boot', 'boot', s.recs[0].time, s.recs[-1].time)


def head(s: Session, pulled: Pulled) -> list[str]:
    first, last = s.recs[0].time, s.recs[-1].time
    fam = families(s)
    state, _ = cpu.kprof(s.recs)
    sym_state = 'loaded' if pulled.syms.active else 'none (pass --dsc for shared-cache symbols)'
    return [
        (
            f'# Boot {s.boot}: {first:%Y-%m-%d %H:%M:%S} to {last:%H:%M:%S} UTC '
            f'({(last - first).total_seconds() / 60:.0f} min), {len(s.recs)} records'
        ),
        'lines read: ' + ', '.join(f'{k} {n}' for k, n in fam.most_common()),
        (
            f'kprof: {state}; probes-kernel tasks lines: {fam["SwitchSched tasks"]}; '
            f'marks: {len(s.windows["mark"])}; symbols: {sym_state}'
        ),
        'windows: ' + ', '.join(f'{k} {len(v)}' for k, v in s.windows.items()),
    ]


def window_rows(s: Session, kind: str, ws: list[Window]) -> list[str]:
    out = []
    for w in ws:
        out.append(f'{kind} {w.name} {w.start:%H:%M:%S} ({w.ms / 1000:.1f} s): {pacing_row(s, w)}')
        if cycle := pacing.periodic(slows_in(s, w)):
            out.append('    ' + cycle)
    return out


def pacing_section(s: Session) -> list[str]:
    out = ['', '## Frame pacing by scenario', 'whole boot: ' + pacing_row(s, whole(s))]
    for kind in ('mark', 'launch', 'touch'):
        out += window_rows(s, kind, s.windows[kind])
    tests = s.windows['test']
    by_lost = sorted(tests, key=lambda w: -sum(x.frame.frames - 1 for x in lost(slows_in(s, w))))
    picked = by_lost[:TOP_TESTS] + [w for w in tests if w.name.startswith(FOCUS) and w not in by_lost[:TOP_TESTS]]
    return out + window_rows(s, 'test', picked)


def slow_section(s: Session, syms: symbols.Symbols) -> list[str]:
    stages: Counter[str] = Counter()
    periods: Counter[str] = Counter()
    for x in s.slows:
        stages[x.stage] += 1
        periods[x.stage] += x.frame.frames - 1
    joined = sum(1 for x in s.slows if x.render)
    out = [
        '',
        '## Slow presents: the stage that lost the period',
        f'{len(s.slows)} slow presents, {joined} joined with their render thread line',
    ]
    out += [f'  {stage}: {n} presents, {periods[stage]} periods' for stage, n in stages.most_common()]
    sites = Counter(x.render.longest for x in s.slows if x.stage == pacing.WAITED and x.render)
    out += [
        f'  the render thread waited at {syms.resolve_where(site)}: {n} presents' for site, n in sites.most_common(4)
    ]
    worst = sorted(lost(s.slows), key=lambda x: -x.frame.frames)[:TIMELINES]
    waited = [x for x in s.slows if x.stage == pacing.WAITED][:WAITED_TIMELINES]
    for x in worst + waited:
        out += pacing.timeline(x)
    return out


def cpu_section(s: Session, syms: symbols.Symbols) -> list[str]:
    out = ['', '## CPU']
    if s.busy:
        cores = sorted(b.cores for b in s.busy)
        out.append(
            f'busy cores per 10 s: p25 {cores[len(cores) // 4]:.2f}, p50 {cores[len(cores) // 2]:.2f}, max '
            f'{cores[-1]:.2f} over {len(cores)} windows'
        )
    top = cpu.top_tasks(whole(s), s.tasks, 12)
    out.append(
        'processes by user+system: ' + ', '.join(f'{n} {v:.0f} ms/s' for n, v in top)
        if top
        else 'processes by CPU: not in this kernel (probes-kernel 1 adds SwitchSched tasks)'
    )
    out += cpu.cpu_floor(s.busy, s.tasks, s.kprof, syms)
    waits = cpu.core_waits(s.recs)
    named = [n for n in FRAME_THREADS if n in waits]
    named += sorted(
        (n for n in waits if n not in named and waits[n].worst_us >= SLOW_WAIT_US), key=lambda n: -waits[n].worst_us
    )
    for name in named:
        w = waits[name]
        out.append(
            f'runnable to running: {name} {w.runs} runs, worst p99 {w.worst_p99} us, max {w.worst_us} us'
            + (f' (window ending {w.when:%H:%M:%S}{", still booting" if w.booting else ""})' if w.when else '')
        )
    state, lines = cpu.kprof(s.recs)
    out.append(f'kprof: {state}')
    out += [f'  {r.time:%H:%M:%S} {r.msg[:300]}' for r in lines[-12:]]
    return out


def work_section(s: Session) -> list[str]:
    ours = [c for c in s.costs if not work.apple(c)]
    marks = [c for c in s.costs if work.apple(c)]
    out = ['', '## State creations, compiles and imports (the asking thread waited this long)']
    out += [row.text() for row in work.by_kind(ours)[:16]]
    out += [f'  {at(c.start)} {c.proc}[{c.pid}] {c.what} {c.ms:.0f} ms' for c in sorted(ours, key=lambda c: -c.ms)[:8]]
    out += ['', "## Apple's own stall marks (signposts the debug logging profile keeps)"]
    out += [row.text() for row in work.by_kind(marks)[:12]]
    return out


def vm_section(s: Session, syms: symbols.Symbols) -> list[str]:
    pi = vm.pagein_summary(whole(s), s.pageins)
    un = vm.unnest_summary(s.unnests, syms)
    if not pi and not un:
        return []
    return ['', '## Page-ins and shared-cache unnests', *pi, *un]


def hang_section(s: Session, pulled: Pulled) -> list[str]:
    hangs = s.windows['hang']
    total: Counter[str] = Counter()
    for h in hangs:
        total[h.proc] += h.ms
    out = ['', '## Hangs (HangTracer: a main thread unresponsive for 250 ms or more)', f'{len(hangs)} hangs']
    out += [f'  {proc}: {ms / 1000:.1f} s' for proc, ms in total.most_common(8)]
    out.append('  by type: ' + ', '.join(f'{t or "?"} {n}' for t, n in Counter(h.note for h in hangs).most_common()))
    in_tests: Counter[str] = Counter()
    for h in hangs:
        for t in s.windows['test']:
            if t.proc == h.proc:
                in_tests[t.name] += t.overlap(h.start, h.end)
    out.append(
        '  inside HW Test tests: ' + ', '.join(f'{k} {v / 1000:.1f} s' for k, v in in_tests.most_common(10) if v)
    )
    by_name = {t.name: t for t in s.windows['test']}
    for name, ms in in_tests.most_common(HANG_TESTS):
        t = by_name[name]
        inside = [h for h in hangs if h.proc == t.proc and t.overlap(h.start, h.end)]
        out.append(f'test {name} ({t.ms / 1000:.1f} s): {len(inside)} hangs, {ms / 1000:.1f} s of them inside it')
        out += ['    ' + line for line in dossier(s, t, pulled.syms)]
    for h in sorted(hangs, key=lambda h: -h.ms)[:HANG_DOSSIERS]:
        tests = [t.name for t in s.windows['test'] if t.overlap(h.start, h.end)]
        out.append(f'{at(h.start)} {h.proc}[{h.pid}] {h.name} {h.note}; tests {", ".join(tests[:3]) or "-"}')
        out += ['    ' + line for line in dossier(s, h, pulled.syms)]
        out.append('    its stack: none (no hang report on this device; the recorder, DESIGN.md S2, takes one)')
    out += [
        '',
        '### Reports pulled with the archive',
        *ips.summary(pulled.reports, {s.boot.upper()}, pulled.images, WATCH, pulled.syms),
    ]
    out += ['', '### Watchdog stackshots in the folder (every boot)', *ips.watchdogs(pulled.reports)]
    status = [r for r in s.recs if 'TailspinStatus' in r.msg or 'microstackshot' in r.msg or 'tailspin' in r.category]
    out.append(
        f'tailspin or spindump files in the folder: {len(pulled.spins)}; what Apple logged about its own capture:'
    )
    out += [f'  {r.time:%H:%M:%S} {r.proc}: {r.msg[:160]}' for r in dedupe(status)[:8]]
    return out


def dedupe(recs: list) -> list:
    seen, out = set(), []
    for r in recs:
        if (r.proc, r.msg) not in seen:
            seen.add((r.proc, r.msg))
            out.append(r)
    return out


def phases(s: Session, w: Window) -> str:
    """Return the launch's steps: the process started, its first Core Animation commit, its content ready."""
    up = next(
        (r.time for r in s.recs if w.holds(r.time) and (m := BOOTSTRAPPED.search(r.msg)) and m['bundle'] == w.name),
        None,
    )
    first = work.first_commit(s.recs, w.pid, w.start, w.end)
    parts = [f'asked {at(w.start)}']
    if up:
        parts.append(f'process up +{(up - w.start).total_seconds():.2f} s')
    if first:
        began, ended = ((t - w.start).total_seconds() for t in first)
        parts.append(f'first commit +{ended:.2f} s, its transaction begun at +{began:.2f} s')
    else:
        parts.append('no Core Animation commit logged')
    parts.append(f'content ready +{w.ms / 1000:.2f} s')
    return '; '.join(parts)


def launch_section(s: Session, syms: symbols.Symbols) -> list[str]:
    out = ['', '## Launches (FrontBoard asked to start it, to its scene content ready)']
    for w in s.windows['launch']:
        out.append(f'{at(w.start)} {w.name}[{w.pid}]: {w.ms / 1000:.2f} s {w.note}'.rstrip())
        if w.ms:
            out.append('    ' + phases(s, w))
            out += ['    ' + line for line in dossier(s, w, syms)]
    return out


def dump_section(s: Session, pulled: Pulled) -> list[str]:
    first, last = s.recs[0].time.timestamp(), s.recs[-1].time.timestamp()
    mine = [(r, k) for r, k in pulled.dumps if first - DUMP_SLACK_S <= ktrace.tod(r) <= last + DUMP_SLACK_S]
    lines = [r for r in s.recs if r.msg.startswith('SwitchRecorder')]
    out = [
        '',
        '## Flight recorder dumps',
        f'{len(lines)} recorder lines in the log, {len(mine)} dumps of this boot pulled',
    ]
    out += [f'  {r.time:%H:%M:%S} {r.msg[:200]}' for r in lines[-8:]]
    for ring, shot in mine:
        try:
            out += dumps.summary(ring, shot, pulled.kcdata, pulled.images, pulled.syms)
        except (ValueError, OSError) as exc:
            out.append(f'{os.path.basename(ring)}: unreadable dump ({exc})')
    return out


def render(s: Session, pulled: Pulled) -> str:
    out = head(s, pulled)
    out += pacing_section(s)
    out += slow_section(s, pulled.syms)
    out += cpu_section(s, pulled.syms)
    out += ['', '## GPU', *gpu.lines(gpu.read(s.recs))]
    out += work_section(s)
    out += vm_section(s, pulled.syms)
    out += hang_section(s, pulled)
    out += dump_section(s, pulled)
    out += launch_section(s, pulled.syms)
    out += ['', '## What these lines cannot answer', *(f'- {x}' for x in LIMITS)]
    return '\n'.join(out) + '\n'
