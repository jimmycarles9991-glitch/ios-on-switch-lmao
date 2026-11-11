"""The rows that come from tools/frames and from a directory of crash reports rather than from one check's lines."""

import json
from collections import Counter
from pathlib import Path

from tools.devicerun.rules import FAIL, INFO, NO_DATA, PASS, Check, Result
from tools.frames import frames

# .ips bug_type: 309 a crash, 210 a panic, 298 a jetsam event (OSAnalytics' header field).
CRASH, PANIC, JETSAM = '309', '210', '298'
# Processes whose crashes a fix promised to end: (row, owner, process).
WATCHED = (
    ('crash.locationd', 'r122-crashsys', 'locationd'),
    ('crash.spotlightknowledged', 'r122-crashsys', 'spotlightknowledged.updater'),
)
INTERVAL_MS = 1000 / 60
# libdispatch reaching a dispatch_sync waiter's on-stack context after its thread has left it: the drain lock handed to
# an owner that names no thread, the next pointer read from an unmapped stack, or the reused stack's voucher (a
# constant CFString) retained, a write into __AUTH_CONST. Frame 0 of that last one is whichever image holds the shared
# LSE stub (SystemConfiguration's interfaceNameCreateWithoutPortSuffix in 24B119), so its caller is what matches.
STALE_WAITER_ASI = ('dispatch_sync called on queue already owned by current thread', 'Invalid workloop owner')


def row(name: str, owner: str, rule: str, verdict: str, observed: str) -> Result:
    return Result(Check(name, owner, '', rule), verdict, observed)


def periods_percentiles(windows: list[dict]) -> tuple[int, int] | None:
    """Return p50 and p99 of present-to-present intervals in refresh periods, from the 1:/2:/3:/4+: histograms."""
    counts = [sum(int(w[k]) for w in windows) for k in ('f1', 'f2', 'f3', 'f4')]
    total = sum(counts)
    if not total:
        return None
    out = []
    for q in (0.5, 0.99):
        seen = 0
        for periods, n in enumerate(counts, start=1):
            seen += n
            if seen >= q * total:
                out.append(periods)
                break
    return out[0], out[1]


def frame_rows(timed_messages: list[str]) -> list[Result]:
    """Return the frame rows: slow presents and the touch windows' intervals (p50/p99 in refresh periods)."""
    owner = 'r30-frames'
    slow = frames.collect(timed_messages)
    windows = [m.groupdict() for line in timed_messages if (m := frames.INTERACTION.search(line))]
    if not slow and not windows:
        return [row('frames.slow-presents', owner, 'report', NO_DATA, 'no SwitchFrame lines (debug kernels)')]
    lost = sum(f.frames - 1 for f in slow)
    causes = Counter(f.cause for f in slow)
    rows = [
        row(
            'frames.slow-presents',
            owner,
            'report',
            INFO,
            f'{len(slow)} slow presents, {lost} periods lost' + ''.join(f'; {c} {n}' for c, n in causes.most_common()),
        )
    ]
    pct = periods_percentiles(windows)
    if pct is None:
        rows.append(row('frames.interaction', owner, 'p99 == 1 period', NO_DATA, 'no SwitchFrames interaction lines'))
        return rows
    worst = max(float(w['worst']) for w in windows)
    observed = (
        f'{len(windows)} touch windows: p50 {pct[0]} p99 {pct[1]} periods '
        f'({pct[0] * INTERVAL_MS:.1f}/{pct[1] * INTERVAL_MS:.1f} ms), worst {worst:.1f} ms'
    )
    rows.append(row('frames.interaction', owner, 'p99 == 1 period', PASS if pct[1] == 1 else FAIL, observed))
    return rows


def ips_header(path: Path) -> tuple[str, str]:
    """Return (bug_type, process) from an .ips file's two JSON parts."""
    with path.open(errors='replace') as f:
        first = f.readline()
        rest = f.read()
    try:
        header = json.loads(first)
    except json.JSONDecodeError:
        return '?', path.stem.split('-', 1)[0]
    name = header.get('app_name') or header.get('name')
    if not name:
        try:
            name = json.loads(rest).get('procName')
        except json.JSONDecodeError:
            name = None
    return str(header.get('bug_type', '?')), name or path.stem.split('-', 1)[0]


def stale_sync_waiter(path: Path) -> bool:
    """Whether a crash report is libdispatch reaching a dispatch_sync waiter's context its thread has left."""
    with path.open(errors='replace') as f:
        f.readline()
        try:
            body = json.loads(f.read())
        except json.JSONDecodeError:
            return False
    asi = ' '.join(' '.join(lines) for lines in (body.get('asi') or {}).values())
    if any(text in asi for text in STALE_WAITER_ASI):
        return True
    threads = body.get('threads') or []
    faulting = body.get('faultingThread')
    if not isinstance(faulting, int) or faulting >= len(threads):
        return False
    symbols = [frame.get('symbol', '') for frame in threads[faulting].get('frames', [])[:2]]
    if symbols[:1] == ['__DISPATCH_WAIT_FOR_ENQUEUER__']:
        return True
    region = next((line for line in body.get('vmRegionInfo', '').splitlines() if line.startswith('--->')), '')
    return symbols[1:] == ['_dispatch_lane_serial_drain'] and '__AUTH_CONST' in region


def crash_rows(directory: str | None) -> list[Result]:
    """Return the crash budget rows: crashes and panics fail, jetsam and the rest are counted per process."""
    owner = 'main'
    if not directory:
        return [row('crash.reports', owner, 'count == 0', NO_DATA, 'no --crashes directory')]
    kinds: dict[str, Counter] = {}
    stale: list[Path] = []
    for path in sorted(Path(directory).rglob('*.ips')):
        bug_type, process = ips_header(path)
        kinds.setdefault(bug_type, Counter())[process] += 1
        if bug_type == CRASH and stale_sync_waiter(path):
            stale.append(path)

    def listed(bug_type: str) -> str:
        counts = kinds.get(bug_type, Counter())
        return f'{sum(counts.values())}: ' + ', '.join(f'{p} x{n}' for p, n in counts.most_common(8))

    rows = []
    for name, bug_type in (('crash.crashes', CRASH), ('crash.panics', PANIC)):
        verdict = FAIL if kinds.get(bug_type) else PASS
        rows.append(row(name, owner, 'count == 0', verdict, listed(bug_type)))
    crashes = kinds.get(CRASH, Counter())
    for name, watcher, process in WATCHED:
        rows.append(row(name, watcher, 'count == 0', FAIL if crashes[process] else PASS, f'{crashes[process]}'))
    newest = max(stale, key=lambda p: p.name.split('-', 1)[-1]).name if stale else ''
    rows.append(
        row(
            'crash.stale-sync-waiter',
            'r132-locationd',
            'count == 0',
            FAIL if stale else PASS,
            f'{len(stale)}' + (f', newest {newest}' if stale else ''),
        )
    )
    rows.append(row('crash.jetsam', owner, 'report', INFO, listed(JETSAM)))
    other = {k: v for k, v in kinds.items() if k not in {CRASH, PANIC, JETSAM}}
    observed = '; '.join(f'type {k}: {sum(v.values())}' for k, v in sorted(other.items())) or 'none'
    rows.append(row('crash.other-reports', owner, 'report', INFO, observed))
    return rows
