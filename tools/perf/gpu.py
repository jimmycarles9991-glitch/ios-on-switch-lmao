"""What the GPU did: busy share, jobs per process, missed vblanks, long spans by state, encode cost and waits.

The busy share is per stats window; the encode cost and the waits in waitUntilScheduled are each process's own.
"""

import re
from collections import Counter, defaultdict
from dataclasses import dataclass, field

from tools.perf.source import Rec

STATS = re.compile(r'^tegra-gm20b: stats (?P<ms>\d+) ms: (?P<submits>\d+) submits .*? busy (?P<busy>\d+)% \(PTIMER')
PMU = re.compile(r'PMU busy (?P<busy>[\d.]+)% over (?P<counted>[\d.]+)% of the window')
JOBS = re.compile(
    r'^tegra-gm20b: jobs (?P<proc>[^:]+): (?P<jobs>\d+) jobs, .*?GPU mean (?P<mean>\d+) max (?P<max>\d+) us'
)
MISSED = re.compile(r'compositor frame missed its vblank by (?P<us>\d+) us: (?P<why>[^(;]+)')
SPAN = re.compile(r'long span (?P<us>\d+) us k(?P<key>[0-9a-f]{16}) (?P<what>.+?) in a (?P<cb>\d+) us command buffer')
RATES = re.compile(
    r': (?P<proc>\S+) gm20b 5s c=(?P<c>\d+)\(.*? enc=(?P<enc>\d+)us/c .*? ws=(?P<wsn>\d+)/(?P<wsus>\d+)us'
)
STATE = re.compile(r'^(?P<name>.+?) (?P<instr>\d+) instr')
SMMU = re.compile(
    r'^tegra-gm20b: smmu (?P<caches>.*); retirement L2 invalidates (?P<inv>\d+) for \d+ objects, (?P<us>\d+) us CPU'
)
CACHES = re.compile(r'tlb (?P<th>\d+) hits (?P<tm>\d+) misses .*?ptc (?P<ph>\d+) hits (?P<pm>\d+) misses')


@dataclass
class Gpu:
    busy: list[int] = field(default_factory=list)  # stamped busy % per stats window
    pmu: list[float] = field(default_factory=list)  # PMU busy % per stats window (probes-kernel)
    jobs: dict[str, list[int]] = field(default_factory=lambda: defaultdict(lambda: [0, 0, 0]))  # jobs, GPU us, max
    missed: Counter[str] = field(default_factory=Counter)
    spans: dict[str, list[int]] = field(default_factory=lambda: defaultdict(lambda: [0, 0, 0]))  # n, us, max
    encode: dict[str, list[int]] = field(
        default_factory=lambda: defaultdict(lambda: [0, 0, 0, 0])
    )  # c, enc us, ws n, us
    smmu: list[tuple[int, int, int, int]] = field(default_factory=list)  # TLB hits, misses, PTC hits, misses a window
    retire: list[tuple[int, int]] = field(default_factory=list)  # retirement L2 invalidates and their CPU us a window


def span_name(m: re.Match) -> str:
    state = STATE.match(m['what'])
    return f'k{m["key"]} {state["name"]} ({state["instr"]} instr)' if state else f'k{m["key"]} {m["what"][:40]}'


def read(recs: list[Rec]) -> Gpu:
    g = Gpu()
    for r in recs:
        msg = r.msg
        if r.proc == 'kernel':
            if m := STATS.match(msg):
                g.busy.append(int(m['busy']))
                if p := PMU.search(msg):
                    g.pmu.append(float(p['busy']))
            elif m := JOBS.match(msg):
                row = g.jobs[m['proc']]
                row[0] += int(m['jobs'])
                row[1] += int(m['jobs']) * int(m['mean'])
                row[2] = max(row[2], int(m['max']))
            elif m := MISSED.search(msg):
                g.missed[m['why'].strip()] += 1
            elif m := SMMU.match(msg):
                g.retire.append((int(m['inv']), int(m['us'])))
                if c := CACHES.search(m['caches']):
                    g.smmu.append((int(c['th']), int(c['tm']), int(c['ph']), int(c['pm'])))
        elif m := SPAN.search(msg):
            row = g.spans[span_name(m)]
            row[0] += 1
            row[1] += int(m['us'])
            row[2] = max(row[2], int(m['us']))
        elif m := RATES.search(msg):
            row = g.encode[m['proc']]
            c = int(m['c'])
            row[0] += c
            row[1] += c * int(m['enc'])
            row[2] += int(m['wsn'])
            row[3] += int(m['wsus'])
    return g


def lines(g: Gpu) -> list[str]:
    out = []
    if g.busy:
        busy = sorted(g.busy)
        out.append(
            f'stamped busy per stats window: p50 {busy[len(busy) // 2]}%, max {busy[-1]}% over {len(busy)} windows'
        )
    out.append(
        f'PMU busy (GR or copy engine working): p50 {sorted(g.pmu)[len(g.pmu) // 2]:.1f}%, max {max(g.pmu):.1f}%'
        if g.pmu
        else 'PMU busy: not in this kernel (probes-kernel 3 adds it)'
    )
    for name, hits, misses in (('TLB', 0, 1), ('PTC', 2, 3)):
        rates = sorted(w[hits] / (w[hits] + w[misses]) * 100 for w in g.smmu if w[hits] + w[misses])
        if rates:
            missed = sum(w[misses] for w in g.smmu)
            out.append(
                f'SMMU {name} hits per stats window: p50 {rates[len(rates) // 2]:.1f}%, worst {rates[0]:.1f}%; '
                f'{missed} misses over {len(rates)} windows'
            )
    if g.retire:
        counts = sorted(n for n, _ in g.retire)
        out.append(
            f'retirement L2 invalidates per stats window: p50 {counts[len(counts) // 2]}, max {counts[-1]}; '
            f'{sum(us for _, us in g.retire) / 1000:.1f} ms of CPU over {len(counts)} windows'
        )
    for proc, (n, us, worst) in sorted(g.jobs.items(), key=lambda kv: -kv[1][1])[:8]:
        per_job = us / max(n, 1) / 1000
        out.append(f'jobs {proc}: {n} jobs, {us / 1e6:.1f} s of GPU, {per_job:.2f} ms a job, max {worst / 1000:.1f} ms')
    for why, n in g.missed.most_common(4):
        out.append(f'compositor missed its vblank {n} times: {why}')
    for name, (n, us, worst) in sorted(g.spans.items(), key=lambda kv: -kv[1][1])[:10]:
        out.append(f'long spans {name}: {n} x, {us / 1000:.0f} ms in all, max {worst / 1000:.1f} ms')
    for proc, (c, enc, wsn, wsus) in sorted(g.encode.items(), key=lambda kv: -kv[1][1])[:8]:
        if c:
            out.append(
                f'encode {proc}: {c} command buffers, {enc / c / 1000:.2f} ms each; waitUntilScheduled {wsn} x, '
                f'{wsus / max(wsn, 1) / 1000:.2f} ms each'
            )
    return out
