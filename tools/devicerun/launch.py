"""App launches: the memory clock around each, the focal app's main thread, page-ins and the cryptex cache, reported.

Debug kernels log "SwitchLaunch:" (kernel/switch/sched/watch.c) when an app RunningBoard made focal first commits, with
both times as milliseconds before the line; every memory-clock move ("SwitchEMC: A -> B kHz",
drivers/SwitchPower/SwitchEMCClock.h) and the clock's residency per rate in each SwitchPower line
(drivers/SwitchPower/SwitchEMC.cpp); the focal app's main thread between two run-loop waits of 250 ms or more
("SwitchBusy: ... (the focal app)"); and, every 10 s, page-ins by process and file ("SwitchSched page-ins") and the
cryptex read cache's counts ("SwitchSched cryptex cache", kernel/switch/diag/pagein.c). Nothing here is judged.
"""

import re
from collections import Counter
from datetime import datetime, timedelta

from tools.devicerun.rules import INFO, NO_DATA, REPORT, Check, Result

KERNEL = 'kernel'
OWNER = 'probes-kernel'
WATCH = 'kernel/switch/sched/watch.c'
PAGEIN = 'kernel/switch/diag/pagein.c'
TOP = 8

LAUNCH = re.compile(
    r'SwitchLaunch: (?P<name>.+?)\[(?P<pid>\d+)\] became the focal app (?P<focal>\d+) ms before this line; its first '
    r'commit (?P<first>\d+) ms before it'
)
MOVE = re.compile(r'SwitchEMC: (?P<old>\d+) -> (?P<new>\d+) kHz')
RESIDENCY = re.compile(r'SwitchPower: (?P<ms>\d+) ms: .*?EMC \d+ moves(?P<rates>(?: \d+:\d+\.\d%)+)')
RATE = re.compile(r' (?P<mhz>\d+):(?P<pct>\d+\.\d)%')
# The rows come last in each line, so a line cut at the log record's size loses only its last rows.
PAGEINS = re.compile(
    r'(?P<waits>\d+) waits for pages being read, (?P<wait>\d+) ms.*?; untracked (?P<untracked>\d+)'
    r'(?:; XNU page-ins (?P<xnu>\d+), (?P<again>\d+) evicted and paged in again[^;]*)?; by process '
    r'\(reads/KiB asked/ms reading/waits/ms waiting\):(?P<processes>.*)'
)
PAGEIN_FILES = re.compile(r'SwitchSched page-ins by file \(\d+ ms; [^)]*\):(?P<files>.*)')
ROW = re.compile(r' ?(?P<name>[^,]+?) (?P<reads>\d+)/(?P<kib>\d+)/(?P<read>\d+)/(?P<waits>\d+)/(?P<wait>\d+)(?:,|$)')
CACHE = re.compile(
    r'SwitchSched cryptex cache \(\d+ ms\): (?P<requests>\d+) requests \((?P<held>\d+) all held, (?P<left>\d+) left to '
    r'the card\); (?P<reads>\d+) card reads of (?P<kib>\d+) KiB, (?P<ahead>\d+) KiB of it read ahead; (?P<served>\d+) '
    r'KiB served the first time \((?P<ahead_served>\d+) KiB of the read-ahead\); (?P<evicted>\d+) windows reused '
    r'holding blocks never served; (?P<failed>\d+) reads failed'
)

LAUNCHES = [
    Check('launch.first-commit', OWNER, 'SwitchLaunch: ', REPORT, process=KERNEL, regex=LAUNCH.pattern, source=WATCH),
    Check(
        'launch.emc-moves',
        OWNER,
        'SwitchEMC: ',
        REPORT,
        process=KERNEL,
        regex=MOVE.pattern,
        source='drivers/SwitchPower/SwitchEMCClock.h',
    ),
    Check(
        'launch.emc-residency',
        OWNER,
        'SwitchPower: ',
        REPORT,
        process=KERNEL,
        regex=RESIDENCY.pattern,
        source='drivers/SwitchPower/SwitchEMC.cpp',
        note="the memory clock's time at each rate in each 10 s window",
    ),
    Check(
        'launch.focal-busy',
        OWNER,
        '(the focal app) busy',
        REPORT,
        process=KERNEL,
        regex=r'SwitchBusy: (?P<name>.+?)\[\d+\] main thread \S+ \(the focal app\) busy (?P<ms>\d+) ms .*?on core '
        r'(?P<cpu>\d+) us, blocked (?P<blocked>\d+) us',
        show=('name',),
        source=WATCH,
        note="the focal app's main thread between two run-loop waits, 250 ms or more, with where it ran and waited",
    ),
    Check(
        'launch.pageins',
        OWNER,
        'SwitchSched page-ins (',
        REPORT,
        process=KERNEL,
        regex=PAGEINS.pattern,
        source=PAGEIN,
        note='file pager reads and the waits for pages a read is filling, by process',
    ),
    Check(
        'launch.pagein-files',
        OWNER,
        'SwitchSched page-ins by file (',
        REPORT,
        process=KERNEL,
        regex=PAGEIN_FILES.pattern,
        source=PAGEIN,
        note='the same by file',
    ),
    Check(
        'launch.cryptex-cache',
        OWNER,
        'SwitchSched cryptex cache (',
        REPORT,
        process=KERNEL,
        regex=CACHE.pattern,
        source='drivers/NativeSwitchBlock/SwitchCryptex.cpp',
    ),
]


def rate_at(moves: list[tuple[datetime, int, int]], at: datetime) -> int | None:
    """Return the memory clock (kHz) in force at `at`: the last move's rate before it, or the first move's old rate."""
    before = [new for when, _, new in moves if when <= at]
    if before:
        return before[-1]
    return moves[0][1] if moves else None


def time_at_rates(moves: list[tuple[datetime, int, int]], start: datetime, end: datetime) -> dict[int, float]:
    """Return the milliseconds between `start` and `end` the memory clock spent at each rate (kHz)."""
    rate = rate_at(moves, start)
    when = start
    spent: Counter[int] = Counter()
    for at, _, new in moves:
        if at <= start or at >= end:
            continue
        if rate is not None:
            spent[rate] += (at - when).total_seconds() * 1000
        when, rate = at, new
    if rate is not None:
        spent[rate] += (end - when).total_seconds() * 1000
    return dict(spent)


def emc_row(launches: list[dict], moves: list[dict]) -> Result:
    check = Check('launch.emc', OWNER, 'SwitchLaunch: ', REPORT, source=WATCH)
    if not launches:
        return Result(check, NO_DATA, 'no SwitchLaunch lines (debug kernels)')
    parts = []
    for f in launches:
        timeline = [(m['time'], int(m['old']), int(m['new'])) for m in moves if m['boot'] == f['boot']]
        focal = f['time'] - timedelta(milliseconds=int(f['focal']))
        first = f['time'] - timedelta(milliseconds=int(f['first']))
        at_focal, at_first = rate_at(timeline, focal), rate_at(timeline, first)
        spent = time_at_rates(timeline, focal, first)
        between = ', '.join(f'{rate // 1000} MHz {ms:.0f} ms' for rate, ms in sorted(spent.items()))
        parts.append(
            f'{f["name"]}[{f["pid"]}] {int(f["focal"]) - int(f["first"])} ms to its first commit, memory clock '
            f'{at_focal // 1000 if at_focal else "?"} MHz at focal, {at_first // 1000 if at_first else "?"} MHz at '
            f'the commit; between: {between or "no move logged"}'
        )
    return Result(check, INFO, f'{len(launches)} launches: ' + '; '.join(parts[:TOP]))


def residency_row(lines: list[dict]) -> Result:
    check = Check(
        'launch.emc-residency-sum', OWNER, 'SwitchPower: ', REPORT, source='drivers/SwitchPower/SwitchEMC.cpp'
    )
    if not lines:
        return Result(check, NO_DATA, 'no SwitchPower lines with the EMC residency (debug kernels)')
    spent: Counter[str] = Counter()
    total = 0
    for f in lines:
        total += int(f['ms'])
        for m in RATE.finditer(f['rates']):
            spent[m['mhz']] += int(f['ms']) * float(m['pct']) / 100
    rates = ', '.join(f'{mhz} MHz {ms / 1000:.1f} s' for mhz, ms in sorted(spent.items(), key=lambda kv: int(kv[0])))
    return Result(check, INFO, f'{len(lines)} windows, {total / 1000:.1f} s: {rates}')


def pagein_row(lines: list[dict], file_lines: list[dict]) -> Result:
    check = Check('launch.pageins-top', OWNER, 'SwitchSched page-ins (', REPORT, source=PAGEIN)
    if not lines:
        return Result(check, NO_DATA, 'no SwitchSched page-ins lines (debug kernels)')
    processes: Counter[str] = Counter()
    files: Counter[str] = Counter()
    for key, into, source in (('processes', processes, lines), ('files', files, file_lines)):
        for f in source:
            for m in ROW.finditer(f.get(key) or ''):
                into[m['name'].strip()] += int(m['read']) + int(m['wait'])
    top = ', '.join(f'{k} {v} ms' for k, v in processes.most_common(TOP))
    named = ', '.join(f'{k} {v} ms' for k, v in files.most_common(TOP))
    waited = sum(int(f['wait']) for f in lines)
    xnu = sum(int(f['xnu'] or 0) for f in lines)
    again = sum(int(f['again'] or 0) for f in lines)
    return Result(
        check,
        INFO,
        f'{len(lines)} windows, {waited} ms waiting for file reads; XNU page-ins {xnu}, {again} of evicted pages paged '
        f'in again; by process: {top}; by file: {named}',
    )


def cache_row(lines: list[dict]) -> Result:
    check = Check('launch.cryptex-cache-sum', OWNER, 'SwitchSched cryptex cache (', REPORT, source=PAGEIN)
    if not lines:
        return Result(check, NO_DATA, 'no SwitchSched cryptex cache lines (debug kernels)')
    sums: Counter[str] = Counter()
    for f in lines:
        sums.update({k: int(v) for k, v in f.items() if k in CACHE.groupindex})
    return Result(
        check,
        INFO,
        f'{len(lines)} windows: {sums["requests"]} requests, {sums["held"]} all held, {sums["left"]} left to the card; '
        f'{sums["reads"]} card reads of {sums["kib"]} KiB ({sums["ahead"]} KiB ahead); {sums["served"]} KiB served '
        f'({sums["ahead_served"]} KiB of the read-ahead); {sums["evicted"]} windows reused unserved; {sums["failed"]} '
        'failed',
    )


def launch_rows(by_check: dict[str, Result]) -> list[Result]:
    """Return the launch, memory-clock, page-in and cryptex cache summaries of the lines the launch checks matched."""

    def lines(name: str) -> list[dict]:
        return by_check[name].matches if name in by_check else []

    return [
        emc_row(lines('launch.first-commit'), lines('launch.emc-moves')),
        residency_row(lines('launch.emc-residency')),
        pagein_row(lines('launch.pageins'), lines('launch.pagein-files')),
        cache_row(lines('launch.cryptex-cache')),
    ]
