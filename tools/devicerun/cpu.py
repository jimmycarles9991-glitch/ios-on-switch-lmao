"""Who used the CPU, which interrupts woke it, what the GPU did: the probe lines, summed up and reported, not judged.

Every line is debug-kernel only: SwitchSched cores and tasks (kernel/switch/sched/cpu.c, with each 10 s scheduling
report), SwitchPower interrupts (drivers/SwitchPower/SwitchPowerStats.cpp), the GPU stats line's PMU figure
(drivers/TegraGM20B/gpu_load.c), SwitchMotion stats (drivers/SwitchMotion/SwitchMotionStats.h) and SwitchUnnest
(kernel/switch/diag/unnest.c). The floor is the quietest quarter of the scheduling windows, by busy cores.
"""

import re
from collections import Counter
from dataclasses import dataclass, field
from datetime import datetime, timedelta

from tools.devicerun.rules import INFO, NO_DATA, REPORT, Check, Result

KERNEL = 'kernel'
OWNER = 'probes-kernel'
CORES = 'SwitchSched cores ('
TASKS = 'SwitchSched tasks ('
IRQS = 'SwitchPower interrupts: '
SCHED_SOURCE = 'kernel/switch/sched/cpu.c'

CORES_WINDOW = re.compile(r'SwitchSched cores \((?P<ms>\d+) ms')
CORE = re.compile(
    r'cpu(?P<cpu>\d+) threads (?P<threads>\d+) \(irq \d+\), idle (?P<idle>\d+) = asleep (?P<asleep>\d+) \+ irq '
    r'(?P<irq>\d+) \+ rest (?P<rest>\d+), (?P<sleeps>\d+) sleeps'
)
TASKS_WINDOW = re.compile(r'SwitchSched tasks \((?P<ms>\d+) ms; [^)]*\): (?P<top>.*?)(?:; )?all (?P<n>\d+) processes')
TASK = re.compile(r'(?P<name>[^,]+?)\[(?P<pid>-?\d+)\] (?P<user>\d+)\+(?P<sys>\d+)')
ALL = re.compile(r'all \d+ processes (?P<user>\d+)\+(?P<sys>\d+), exited \d+ \((?P<xuser>\d+)\+(?P<xsys>\d+)')
IRQS_WINDOW = re.compile(r'SwitchPower interrupts: (?P<ms>\d+) ms')
SOURCE = re.compile(r'(?P<intid>\d+) \(spi \d+\) (?P<count>\d+) \(')
A64X = re.compile(r'a64x traps (?P<traps>\d+), unhandled (?P<unhandled>\d+), text pages (?P<pages>\d+)')
PAIR = timedelta(seconds=2)
TOP = 8

CPU = [
    Check('cpu.cores', OWNER, CORES, REPORT, process=KERNEL, source=SCHED_SOURCE),
    Check('cpu.tasks', OWNER, TASKS, REPORT, process=KERNEL, source=SCHED_SOURCE),
    Check(
        'cpu.interrupts',
        OWNER,
        IRQS,
        REPORT,
        process=KERNEL,
        source='drivers/SwitchPower/SwitchPowerStats.cpp',
    ),
    Check(
        'gpu.pmu-busy',
        OWNER,
        'tegra-gm20b: stats ',
        REPORT,
        process=KERNEL,
        regex=r'PMU busy (?P<busy>[\d.]+)% over (?P<counted>[\d.]+)% of the window \(counter at (?P<mhz>\d+) MHz',
        show=('mhz',),
        source='drivers/TegraGM20B/gpu_load.c',
        note='GR + CE2 busy from the PMU idle counters, per 5 s; the counter rate says which clock they count',
    ),
    Check(
        'motion.windows',
        OWNER,
        'SwitchMotion stats: ',
        REPORT,
        process=KERNEL,
        regex=r'accel (?P<accel>\d) \(client (?P<accel_us>\d+) us.*?gyro (?P<gyro>\d) \(client (?P<gyro_us>\d+) us.*?'
        r'detector (?P<detector>\d), panel lit (?P<lit>\d)',
        show=('lit', 'detector', 'accel_us', 'gyro_us'),
        source='drivers/SwitchMotion/SwitchMotionStats.h',
        note='10 s of converting, by panel state and client intervals: converting with the panel off names its client',
    ),
    Check(
        'vm.unnest',
        OWNER,
        'SwitchUnnest: ',
        REPORT,
        process=KERNEL,
        regex=r'call (?P<call>-?\d+) from user pc (?P<pc>.+?) lr (?P<lr>.+?); kernel',
        show=('call', 'lr'),
        source='kernel/switch/diag/unnest.c',
        note='who unnests the shared region: the call (x16) and the user code that made it',
    ),
]


@dataclass
class Window:
    time: datetime
    boot: str
    ms: int
    busy: float = 0.0  # cores: each core's window less its sleep
    threads: float = 0.0
    idle_irq: float = 0.0
    idle_rest: float = 0.0
    sleeps: int = 0
    tasks: dict[str, float] = field(default_factory=dict)  # name: user + system ms
    attributed: float | None = None  # cores: every process's user + system, the exited included


def core_window(fields: dict) -> Window | None:
    m = CORES_WINDOW.search(fields['message'])
    if not m or not int(m['ms']):
        return None
    w = Window(fields['time'], fields['boot'], int(m['ms']))
    for c in CORE.finditer(fields['message']):
        w.busy += (w.ms - int(c['asleep'])) / w.ms
        w.threads += int(c['threads']) / w.ms
        w.idle_irq += int(c['irq']) / w.ms
        w.idle_rest += int(c['rest']) / w.ms
        w.sleeps += int(c['sleeps'])
    return w


def add_tasks(w: Window, message: str) -> None:
    m = TASKS_WINDOW.search(message)
    total = ALL.search(message)
    if not m or not total:
        return
    for t in TASK.finditer(m['top']):
        w.tasks[t['name'].strip()] = int(t['user']) + int(t['sys'])
    w.attributed = sum(int(total[k]) for k in ('user', 'sys', 'xuser', 'xsys')) / w.ms


def windows(cores: list[dict], tasks: list[dict]) -> list[Window]:
    """Each cores line with the tasks line logged right after it in the same boot."""
    out = [w for f in cores if (w := core_window(f))]
    for w in out:
        line = next((t for t in tasks if t['boot'] == w.boot and w.time <= t['time'] <= w.time + PAIR), None)
        if line:
            add_tasks(w, line['message'])
    return out


def quantile(xs: list[float], q: float) -> float:
    ordered = sorted(xs)
    return ordered[min(len(ordered) - 1, int(q * len(ordered)))]


def floor_row(ws: list[Window]) -> Result:
    check = Check('cpu.floor', OWNER, CORES, REPORT, source=SCHED_SOURCE)
    if not ws:
        return Result(check, NO_DATA, 'no SwitchSched cores lines (debug kernels)')
    p25 = quantile([w.busy for w in ws], 0.25)
    quiet = [w for w in ws if w.busy <= p25]
    n = len(quiet)
    named = [w.attributed for w in quiet if w.attributed is not None]
    wakes = sum(w.sleeps for w in quiet) / sum(w.ms for w in quiet) * 1000
    observed = (
        f'{len(ws)} windows, busy cores p25 {p25:.2f} p50 {quantile([w.busy for w in ws], 0.5):.2f}; the floor '
        f'({n} windows at or below p25): threads {sum(w.threads for w in quiet) / n:.2f} + interrupts while idle '
        f'{sum(w.idle_irq for w in quiet) / n:.2f} + idle path {sum(w.idle_rest for w in quiet) / n:.2f} cores, '
        f'{wakes:.0f} sleeps/s; processes account for '
        + (f'{sum(named) / len(named):.2f} cores' if named else 'nothing (no tasks lines)')
    )
    return Result(check, INFO, observed)


def top_tasks_row(ws: list[Window]) -> Result:
    check = Check('cpu.floor-tasks', OWNER, TASKS, REPORT, source=SCHED_SOURCE)
    if not ws:
        return Result(check, NO_DATA, 'no SwitchSched tasks lines (debug kernels)')
    p25 = quantile([w.busy for w in ws], 0.25)
    quiet = [w for w in ws if w.busy <= p25 and w.tasks]
    if not quiet:
        return Result(check, NO_DATA, 'no tasks line beside a floor window')
    used: Counter[str] = Counter()
    for w in quiet:
        used.update(w.tasks)
    seconds = sum(w.ms for w in quiet) / 1000
    parts = [f'{name} {ms / seconds:.0f} ms/s' for name, ms in used.most_common(TOP)]
    return Result(check, INFO, f'{len(quiet)} floor windows, top by user+system: ' + ', '.join(parts))


def interrupts_row(lines: list[dict]) -> Result:
    check = Check('cpu.interrupt-sources', OWNER, IRQS, REPORT, source='drivers/SwitchPower/SwitchPowerStats.cpp')
    counts: Counter[int] = Counter()
    ms = 0
    for f in lines:
        m = IRQS_WINDOW.search(f['message'])
        if not m:
            continue
        ms += int(m['ms'])
        for s in SOURCE.finditer(f['message']):
            counts[int(s['intid'])] += int(s['count'])
    if not ms:
        return Result(check, NO_DATA, 'no SwitchPower interrupts lines (debug kernels)')
    parts = [f'INTID {i} (SPI {i - 32}) {n * 1000 / ms:.0f}/s' for i, n in counts.most_common(TOP)]
    return Result(check, INFO, f'{len(lines)} windows, the busiest INTIDs: ' + ', '.join(parts))


def a64x_row(cores: list[dict]) -> Result:
    check = Check('cpu.a64x', OWNER, CORES, REPORT, source=SCHED_SOURCE)
    found = [m for f in cores if (m := A64X.search(f['message']))]
    if not found:
        return Result(check, NO_DATA, 'no a64x counts (iOS 27 debug kernels only)')
    traps, unhandled, pages = (sum(int(m[k]) for m in found) for k in ('traps', 'unhandled', 'pages'))
    return Result(
        check, INFO, f'{len(found)} windows: {traps} traps, {unhandled} not emulated, {pages} text pages translated'
    )


def cpu_rows(by_check: dict[str, Result]) -> list[Result]:
    """Return the summaries of the cores, tasks and interrupts lines the CPU checks matched."""

    def lines(name: str) -> list[dict]:
        return by_check[name].matches if name in by_check else []

    ws = windows(lines('cpu.cores'), lines('cpu.tasks'))
    return [floor_row(ws), top_tasks_row(ws), interrupts_row(lines('cpu.interrupts')), a64x_row(lines('cpu.cores'))]
