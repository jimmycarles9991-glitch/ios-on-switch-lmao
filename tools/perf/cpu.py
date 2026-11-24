"""Who had the CPU: busy cores per 10 s, the processes behind them, and how long the frame's threads waited for a core.

Busy cores come from SwitchPower (every debug boot); processes from probes-kernel's SwitchSched cores and tasks lines
when the kernel has them; runnable-to-running from SwitchSched 1/2 and 2/2; where the time went inside a process from
kprof, a measurement kernel's lines.
"""

from __future__ import annotations

import re
from collections import Counter
from dataclasses import dataclass, field
from datetime import datetime, timedelta

from tools.devicerun.cpu import windows as task_windows
from tools.perf import symbols
from tools.perf.source import Rec
from tools.perf.windows import Window

POWER = re.compile(r'^SwitchPower: (?P<ms>\d+) ms: ')
CORE_BUSY = re.compile(r'cpu\d c7 [\d.]+% \([^)]*\) wfi [\d.]+% busy (?P<busy>[\d.]+)%')
EMC = re.compile(r'EMC \d+ moves (?P<rates>(?:\d+:[\d.]+% ?)+)')
EMC_RATE = re.compile(r'(?P<mhz>\d+):(?P<share>[\d.]+)%')
THREAD = re.compile(
    r'(?P<name>[\w.\-]+)\[(?P<pid>\d+)\] [^:;]*: (?P<runs>\d+) runs (?P<p50>\d+)/(?P<p99>\d+)/(?P<max>\d+) us'
)
KPROF_KEEP = ('SwitchKprof tasks', 'SwitchKprof user', 'SwitchKprof waits', 'SwitchKprof images')
KPROF_HEAD = re.compile(r'^SwitchKprof: (?P<ms>\d+) ms, ')
KPROF_TASK = re.compile(r'(?P<name>[^,;]+?)\[(?P<pid>-?\d+)\] (?P<user>\d+)\+(?P<kernel>\d+)=(?P<ms>\d+)ms')
KPROF_USER_PREFIX = re.compile(r'^SwitchKprof user \d+/\d+:\s*')
KPROF_USER_ROLE = re.compile(
    r'^(?P<name>[^\[\]]+?)\[(?P<pid>-?\d+)\] (?P<total>\d+) '
    r'\(main (?P<main>\d+), worker (?P<worker>\d+)\):(?P<rest>.*)$'
)
KPROF_USER_PLAIN = re.compile(r'^(?P<name>[^\[\]]+?)\[(?P<pid>-?\d+)\] (?P<total>\d+):(?P<body>.*)$')
KPROF_ROLE_SEG = re.compile(r'(?P<role>main|worker):(?P<body>[^;]*)')
KPROF_SPOT = re.compile(r'(?P<kind>dsc|main\+)\s+(?P<addr>[0-9a-fA-F]+)\s+(?P<n>\d+)')
KPROF_OTHER = re.compile(r'other\s+(?P<n>\d+)')
KPROF_IMAGES = re.compile(
    r'^SwitchKprof images: (?P<name>.+?)\[(?P<pid>-?\d+)\] '
    r'(?:(?P<role>main|worker) (?P<rsamp>\d+)/)?(?P<total>\d+):(?P<body>.*?); lost (?P<lost>\d+)'
)
KPROF_IMG_ITEM = re.compile(r'(?P<kind>dsc|main\+|other)\s+(?P<mb>[0-9a-fA-F]+)\s+(?P<n>\d+)')
KPROF_GROUP_GAP = timedelta(seconds=2)


@dataclass
class Busy:
    end: datetime
    ms: int
    cores: float
    emc: dict[int, float]  # the memory clock's residency in the window: MHz: share


@dataclass
class Waits:
    runs: int = 0
    worst_p99: int = 0
    worst_us: int = 0
    when: datetime | None = None  # the 10 s window that had the longest wait
    booting: bool = False  # that window ended before System Boot Complete


@dataclass(frozen=True)
class KprofTask:
    name: str
    pid: int
    user: int
    kernel: int
    cpu_ms: int


@dataclass(frozen=True)
class KprofSpot:
    kind: str  # 'dsc' or 'main+'
    addr: int
    samples: int


@dataclass(frozen=True)
class KprofUser:
    name: str
    pid: int
    role: str  # 'main', 'worker', or '' (legacy unsplit line)
    samples: int
    spots: tuple[KprofSpot, ...]
    other: int


@dataclass(frozen=True)
class KprofImage:
    name: str
    pid: int
    role: str
    samples: int
    regions: tuple[KprofSpot, ...]


@dataclass
class KprofWindow:
    time: datetime
    ms: int = 10000
    tasks: list[KprofTask] = field(default_factory=list)
    users: list[KprofUser] = field(default_factory=list)
    images: list[KprofImage] = field(default_factory=list)


def busy_windows(recs: list[Rec]) -> list[Busy]:
    out = []
    for r in recs:
        if r.proc == 'kernel' and (m := POWER.match(r.msg)):
            cores = sum(float(c['busy']) for c in CORE_BUSY.finditer(r.msg)) / 100
            rates = EMC.search(r.msg)
            emc = {int(e['mhz']): float(e['share']) / 100 for e in EMC_RATE.finditer(rates['rates'])} if rates else {}
            out.append(Busy(r.time, int(m['ms']), cores, emc))
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
    return out


def kprof(recs: list[Rec]) -> tuple[str, list[Rec]]:
    """Return kprof's state in this boot (off, absent or on) and its per-window lines about processes."""
    lines = [r for r in recs if r.proc == 'kernel' and r.msg.startswith('SwitchKprof')]
    if not lines:
        return 'absent', []
    if any(r.msg.startswith('SwitchKprof: off') for r in lines):
        return 'off (built without --//kernel:kprof)', []
    return 'on', [r for r in lines if r.msg.startswith(KPROF_KEEP)]


def _parse_spots(body: str) -> tuple[tuple[KprofSpot, ...], int]:
    spots = tuple(KprofSpot(m['kind'], int(m['addr'], 16), int(m['n'])) for m in KPROF_SPOT.finditer(body))
    other = int(m['n']) if (m := KPROF_OTHER.search(body)) else 0
    return spots, other


def _parse_kprof_user(msg: str) -> list[KprofUser]:
    rest = KPROF_USER_PREFIX.sub('', msg, count=1).strip()
    if m := KPROF_USER_ROLE.match(rest):
        name, pid = m['name'].strip(), int(m['pid'])
        counts = {'main': int(m['main']), 'worker': int(m['worker'])}
        out = []
        for seg in KPROF_ROLE_SEG.finditer(m['rest']):
            role = seg['role']
            spots, other = _parse_spots(seg['body'])
            out.append(KprofUser(name, pid, role, counts[role], spots, other))
        return out
    out = []
    for chunk in rest.split(';'):
        if pm := KPROF_USER_PLAIN.match(chunk.strip()):
            spots, other = _parse_spots(pm['body'])
            out.append(KprofUser(pm['name'].strip(), int(pm['pid']), '', int(pm['total']), spots, other))
    return out


def _parse_kprof_images(msg: str) -> KprofImage | None:
    m = KPROF_IMAGES.match(msg)
    if not m:
        return None
    regions = tuple(KprofSpot(it['kind'], int(it['mb'], 16), int(it['n'])) for it in KPROF_IMG_ITEM.finditer(m['body']))
    samples = int(m['rsamp']) if m['rsamp'] else int(m['total'])
    return KprofImage(m['name'].strip(), int(m['pid']), m['role'] or '', samples, regions)


def kprof_windows(recs: list[Rec]) -> list[KprofWindow]:
    """Group SwitchKprof lines into one KprofWindow per 10 s summary burst."""
    out: list[KprofWindow] = []

    def current(t: datetime, ms: int = 10000) -> KprofWindow:
        if not out or abs(t - out[-1].time) > KPROF_GROUP_GAP:
            out.append(KprofWindow(t, ms))
        return out[-1]

    for r in recs:
        if r.proc != 'kernel' or not r.msg.startswith('SwitchKprof'):
            continue
        if m := KPROF_HEAD.match(r.msg):
            out.append(KprofWindow(r.time, int(m['ms'])))
        elif r.msg.startswith('SwitchKprof tasks'):
            body = r.msg.split(':', 1)[1] if ':' in r.msg else ''
            w = current(r.time)
            w.tasks += [
                KprofTask(tm['name'].strip(), int(tm['pid']), int(tm['user']), int(tm['kernel']), int(tm['ms']))
                for tm in KPROF_TASK.finditer(body)
            ]
        elif r.msg.startswith('SwitchKprof user '):
            current(r.time).users += _parse_kprof_user(r.msg)
        elif img := _parse_kprof_images(r.msg):
            current(r.time).images.append(img)
    return [w for w in out if w.tasks or w.users or w.images]


def _fmt_spot(proc: str, spot: KprofSpot, syms: symbols.Symbols | None) -> str:
    if spot.kind == 'dsc':
        return syms.dsc_addr(spot.addr) if syms else f'dsc 0x{spot.addr:x}'
    if spot.kind == 'main+':
        return syms.image_addr(proc, spot.addr) if syms else f'main+0x{spot.addr:x}'
    return 'other'


def _fmt_region(proc: str, reg: KprofSpot, syms: symbols.Symbols | None) -> str:
    if reg.kind == 'dsc':
        return syms.dsc_region(reg.addr) if syms else f'dsc {reg.addr:x}'
    if reg.kind == 'main+':
        return f'{proc}+0x{reg.addr << 20:x}'
    return 'other'


def _kprof_hot_lines(
    ws: list[KprofWindow],
    syms: symbols.Symbols | None,
    focus_proc: str,
    limit: int = 4,
) -> list[str]:
    by_pr: dict[tuple[str, str], Counter[tuple[str, int]]] = {}
    role_totals: Counter[tuple[str, str]] = Counter()
    for kw in ws:
        for u in kw.users:
            key = (u.name, u.role)
            role_totals[key] += u.samples
            bag = by_pr.setdefault(key, Counter())
            for sp in u.spots:
                bag[(sp.kind, sp.addr)] += sp.samples
    ordered = sorted(
        role_totals.items(),
        key=lambda kv: (0 if focus_proc and kv[0][0] == focus_proc else 1, -kv[1], kv[0]),
    )
    out = []
    for (proc, role), total in ordered[:limit]:
        spots = by_pr.get((proc, role), Counter()).most_common(4)
        if not spots:
            continue
        tag = f' ({role})' if role else ''
        text = ', '.join(f'{_fmt_spot(proc, KprofSpot(k, a, n), syms)} {n}' for (k, a), n in spots)
        out.append(f'kprof user {proc}{tag} ({total} samples): {text}')
    return out


def _kprof_image_lines(
    ws: list[KprofWindow],
    syms: symbols.Symbols | None,
    focus_proc: str,
    limit: int = 2,
) -> list[str]:
    by_pr: dict[tuple[str, str], Counter[tuple[str, int]]] = {}
    totals: Counter[tuple[str, str]] = Counter()
    for kw in ws:
        for im in kw.images:
            key = (im.name, im.role)
            totals[key] += im.samples
            bag = by_pr.setdefault(key, Counter())
            for reg in im.regions:
                bag[(reg.kind, reg.addr)] += reg.samples
    ordered = sorted(
        totals.items(),
        key=lambda kv: (0 if focus_proc and kv[0][0] == focus_proc else 1, -kv[1], kv[0]),
    )
    out = []
    for (proc, role), total in ordered[:limit]:
        regs = by_pr.get((proc, role), Counter()).most_common(4)
        if not regs:
            continue
        tag = f' ({role})' if role else ''
        text = ', '.join(f'{_fmt_region(proc, KprofSpot(k, a, n), syms)} {n}' for (k, a), n in regs)
        out.append(f'kprof images {proc}{tag} ({total} samples): {text}')
    return out


def kprof_in_window(
    w: Window,
    ws: list[KprofWindow],
    syms: symbols.Symbols | None = None,
    focus_proc: str = '',
) -> list[str]:
    """Summarize SwitchKprof tasks, hot spots and regions overlapping one window."""
    chosen = [kw for kw in ws if w.holds(kw.time)]
    if not chosen:
        chosen = [kw for kw in ws if w.overlap(kw.time - timedelta(milliseconds=kw.ms), kw.time)]
    if not chosen:
        return []
    cpu_ms: Counter[str] = Counter()
    samples: Counter[str] = Counter()
    seconds = sum(kw.ms for kw in chosen) / 1000
    for kw in chosen:
        for t in kw.tasks:
            cpu_ms[t.name] += t.cpu_ms
            samples[t.name] += t.user + t.kernel
    out = []
    if cpu_ms and seconds:
        out.append(
            'kprof tasks: '
            + ', '.join(f'{name} {ms / seconds:.0f} ms/s ({samples[name]}s)' for name, ms in cpu_ms.most_common(6))
        )
    out += _kprof_hot_lines(chosen, syms, focus_proc)
    out += _kprof_image_lines(chosen, syms, focus_proc)
    return out


def _floor_spans(busy: list[Busy], sched_tasks: list, kprof_ws: list[KprofWindow]) -> tuple[list[Window], float | None]:
    if busy:
        n = max(1, len(busy) // 4)
        picked = sorted(busy, key=lambda b: b.cores)[:n]
        spans = [Window('floor', 'floor', b.end - timedelta(milliseconds=b.ms), b.end) for b in picked]
        return spans, sum(b.cores for b in picked) / len(picked)
    if sched_tasks:
        n = max(1, len(sched_tasks) // 4)
        picked = sorted(sched_tasks, key=lambda tw: tw.busy)[:n]
        spans = [Window('floor', 'floor', tw.time - timedelta(milliseconds=tw.ms), tw.time) for tw in picked]
        return spans, sum(tw.busy for tw in picked) / len(picked)
    if kprof_ws:
        n = max(1, len(kprof_ws) // 4)
        picked = sorted(kprof_ws, key=lambda kw: sum(t.cpu_ms for t in kw.tasks))[:n]
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
    sched_used: Counter[str] = Counter()
    sched_s = 0.0
    for tw in sched_tasks:
        if tw.tasks and any(sp.overlap(tw.time - timedelta(milliseconds=tw.ms), tw.time) for sp in spans):
            sched_used.update(tw.tasks)
            sched_s += tw.ms / 1000
    if sched_used and sched_s:
        out.append(
            '  floor processes (SwitchSched tasks): '
            + ', '.join(f'{n} {v / sched_s:.0f} ms/s' for n, v in sched_used.most_common(8))
        )
    floor_kp = [
        kw for kw in kprof_ws if any(sp.overlap(kw.time - timedelta(milliseconds=kw.ms), kw.time) for sp in spans)
    ]
    if floor_kp:
        kp_ms: Counter[str] = Counter()
        kp_s = sum(kw.ms for kw in floor_kp) / 1000
        for kw in floor_kp:
            for t in kw.tasks:
                kp_ms[t.name] += t.cpu_ms
        if kp_ms and kp_s:
            out.append(
                '  floor processes (SwitchKprof): '
                + ', '.join(f'{n} {v / kp_s:.0f} ms/s' for n, v in kp_ms.most_common(8))
            )
        out += ['  ' + line for line in _kprof_hot_lines(floor_kp, syms, '', limit=6)]
        out += ['  ' + line for line in _kprof_image_lines(floor_kp, syms, '', limit=4)]
    return out
