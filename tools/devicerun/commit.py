"""Why commits wait: the debug kernel's commit and touch-delivery lines, summed up and reported, not judged.

kernel/switch/sched/watch.c logs a "SwitchCommit:" line for each commit a client's thread took a refresh period or more
to reach (from the wake that ended its idle wait, or from its commit before), a "SwitchTouchSend:" line for each touch
report backboardd sent on 8 ms or more after its interrupt, and with each 10 s scheduling report the window's counts
("SwitchSched watch") and the commits' way through the render server to the display thread's wake ("SwitchSched commit
path"). "mostly" is where most of the stretch went: on a core, waiting for one, or blocked.
"""

import re
from collections import Counter, defaultdict

from tools.devicerun.rules import INFO, NO_DATA, REPORT, Check, Result

KERNEL = 'kernel'
OWNER = 'probes-kernel'
SOURCE = 'kernel/switch/sched/watch.c'
WINDOW = 'SwitchSched watch ('
PATH = 'SwitchSched commit path ('
TOP = 8

STRETCH = (
    r'(?P<ms>[\d.]+) ms since (?P<since>its previous one|its idle wait ended) \(the one before (?P<gap>\d+) ms '
    r'earlier\), mostly (?P<cause>on core|waiting for a core|blocked): on core (?P<cpu>\d+) us, '
    r'blocked (?P<blocked>\d+) us in \d+ waits \((?P<kinds>[^)]*)\), waiting for a core (?P<core>\d+) us'
)
WINDOW_COUNTS = re.compile(
    r'commits (?P<commits>\d+), (?P<late>\d+) late \(on core (?P<cpu>\d+), waiting for a core (?P<core>\d+), blocked '
    r'(?P<blocked>\d+)\); touch deliveries (?P<touches>\d+), (?P<touch_late>\d+) late .*?; (?P<dropped>\d+) late not '
    r'logged'
)
PATH_SPLIT = re.compile(
    r'(?P<n>\d+) commits to the frame\'s wake, p50/p99 us: received (?P<r50>\d+)/(?P<r99>\d+), applied '
    r'(?P<a50>\d+)/(?P<a99>\d+), waited (?P<w50>\d+)/(?P<w99>\d+), all (?P<t50>\d+)/(?P<t99>\d+)'
)
# A kind's name can hold a comma ("mach_msg send, in transit"): it runs to its " <us> us/<n>".
KIND = re.compile(r'(?:^|, )(?P<kind>.+?) (?P<us>\d+) us/\d+(?=, |$)')

COMMIT = [
    Check(
        'commit.late',
        OWNER,
        'SwitchCommit: ',
        REPORT,
        process=KERNEL,
        regex=r'SwitchCommit: (?P<name>.+?)\[\d+\] thread \S+ (?P<pri>\d+)/\d+ (?P<mode>\S+) \S+: ' + STRETCH,
        show=('name', 'cause'),
        source=SOURCE,
        note='a commit 16.7 ms or more after its thread woke from its idle wait (or after its commit before)',
    ),
    Check(
        'commit.window',
        OWNER,
        WINDOW,
        REPORT,
        process=KERNEL,
        regex=WINDOW_COUNTS.pattern,
        source=SOURCE,
    ),
    Check('commit.path', OWNER, PATH, REPORT, process=KERNEL, regex=PATH_SPLIT.pattern, source=SOURCE),
    Check(
        'touch.send-late',
        OWNER,
        'SwitchTouchSend: ',
        REPORT,
        process=KERNEL,
        regex=r'SwitchTouchSend: \S+ thread (?P<thread>\S+) (?P<pri>\d+)/\d+ (?P<mode>\S+) \S+ to pid \d+: '
        r'report to send (?P<us>\d+) us, the stretch began (?P<lead>-?\d+) us after the report; ' + STRETCH,
        show=('thread', 'pri', 'cause'),
        source=SOURCE,
        note='a touch report sent on 8 ms or more after its interrupt, by which backboardd thread and why',
    ),
]


def blocked_kinds(fields: dict) -> Counter[str]:
    return Counter({m['kind'].strip(): int(m['us']) for m in KIND.finditer(fields.get('kinds', ''))})


def late_row(lines: list[dict]) -> Result:
    check = Check('commit.wait-by-cause', OWNER, 'SwitchCommit: ', REPORT, source=SOURCE)
    if not lines:
        return Result(check, NO_DATA, 'no SwitchCommit lines (debug kernels; none late, or no client committed)')
    by_name: dict[str, Counter[str]] = defaultdict(Counter)
    blocked: Counter[str] = Counter()
    total_ms = 0.0
    for f in lines:
        by_name[f['name']][f['cause']] += 1
        total_ms += float(f['ms'])
        blocked.update(blocked_kinds(f))
    ranked = sorted(by_name.items(), key=lambda kv: -sum(kv[1].values()))[:TOP]
    parts = [
        f'{name} {sum(c.values())} (' + ', '.join(f'{k} {n}' for k, n in c.most_common()) + ')' for name, c in ranked
    ]
    kinds = ', '.join(f'{k} {us // 1000} ms' for k, us in blocked.most_common(4))
    return Result(
        check,
        INFO,
        f'{len(lines)} late commits, {total_ms / len(lines):.1f} ms mean: '
        + '; '.join(parts)
        + f'; blocked in: {kinds}',
    )


def window_row(lines: list[dict]) -> Result:
    check = Check('commit.late-share', OWNER, WINDOW, REPORT, source=SOURCE)
    sums: Counter[str] = Counter()
    for f in lines:
        sums.update({k: int(v) for k, v in f.items() if k in WINDOW_COUNTS.groupindex})
    if not sums['commits']:
        return Result(check, NO_DATA, 'no SwitchSched watch lines with commits (debug kernels)')
    return Result(
        check,
        INFO,
        f'{len(lines)} windows: {sums["late"]} of {sums["commits"]} commits late (on core {sums["cpu"]}, waiting for a '
        f'core {sums["core"]}, blocked {sums["blocked"]}); {sums["touch_late"]} of {sums["touches"]} touch deliveries '
        f'late; {sums["dropped"]} late not logged',
    )


def path_row(lines: list[dict]) -> Result:
    check = Check('commit.path-split', OWNER, PATH, REPORT, source=SOURCE)
    windows = [f for f in lines if int(f.get('n', 0))]
    if not windows:
        return Result(check, NO_DATA, 'no SwitchSched commit path lines with commits (debug kernels)')

    def median(key: str) -> int:
        ordered = sorted(int(f[key]) for f in windows)
        return ordered[len(ordered) // 2]

    return Result(
        check,
        INFO,
        f"{len(windows)} windows, {sum(int(f['n']) for f in windows)} commits; median of the windows' p50 us: received "
        f'{median("r50")}, applied {median("a50")}, waited for the frame {median("w50")}, all {median("t50")}; worst '
        f'window p99 all {max(int(f["t99"]) for f in windows)} us',
    )


def commit_rows(by_check: dict[str, Result]) -> list[Result]:
    """Return the summaries of the commit and touch-delivery lines the commit checks matched."""

    def lines(name: str) -> list[dict]:
        return by_check[name].matches if name in by_check else []

    return [late_row(lines('commit.late')), window_row(lines('commit.window')), path_row(lines('commit.path'))]
