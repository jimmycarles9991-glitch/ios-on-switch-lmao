"""The rows that come from tools/frames and from a directory of crash reports rather than from one check's lines."""

import json
import re
from collections import Counter
from pathlib import Path

from tools.devicerun.hwtest import OWNER, REDS, SKIPS, SUBSYSTEM
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


def line_fields(message: str) -> dict[str, str]:
    """Return a HWTEST line's fields before why= (apps/hwtest/core/LogLine.swift: one space apart, why= last)."""
    fields = {}
    for part in message.split(' ')[3:]:
        key, _, value = part.partition('=')
        if key == 'why':
            break
        fields[key] = value
    return fields


def predicates(text: str) -> list[str]:
    """Return the predicates of a "pred/n,pred/n" field (FailedCounts.text)."""
    return [item.rpartition('/')[0] for item in text.split(',') if '/' in item]


def red_rows(matches: list[dict], boot: str) -> list[Result]:
    """Return the first test a failed command buffer reddened and the tests each predicate reddened, in `boot`.

    The first test with its cause, and per predicate the first of its tests, own or foreign.
    """
    lines = [f for f in matches if f['boot'] == boot]
    if not lines:
        observed = f'boot {boot or "-"}: no test reddened by a failed command buffer'
        return [row('hwtest.first-red', OWNER, 'report', NO_DATA, observed)]
    reds: dict[str, tuple[str, str, set[str]]] = {}
    for fields in lines:
        named = line_fields(fields['message'])
        for whose in ('own', 'foreign'):
            for predicate in predicates(named.get(f'{whose}_why', '')):
                reds.setdefault(predicate, (fields['id'], whose, set()))[2].add(fields['id'])
    first = line_fields(lines[0]['message'])
    cause = ', '.join(f'{whose} {first[whose + "_why"]}' for whose in ('own', 'foreign') if whose + '_why' in first)
    rows = [row('hwtest.first-red', OWNER, 'report', INFO, f'boot {boot}: {lines[0]["id"]} ({cause})')]
    for predicate, (test, whose, tests) in reds.items():
        observed = f'{len(tests)} test{"s" * (len(tests) != 1)} reddened, first {test} ({whose})'
        rows.append(row(f'hwtest.red.{predicate}', OWNER, 'report', INFO, observed))
    return rows


def skip_rows(matches: list[dict], boot: str) -> list[Result]:
    """Return per predicate of a skipped call its tests and the first, in `boot`."""
    lines = [f for f in matches if f['boot'] == boot]
    if not lines:
        return [row('hwtest.skipped-calls', OWNER, 'report', NO_DATA, f'boot {boot or "-"}: no skipped call')]
    skips: dict[str, tuple[str, set[str]]] = {}
    for fields in lines:
        for predicate in predicates(line_fields(fields['message']).get('skipped_why', '')):
            skips.setdefault(predicate, (fields['id'], set()))[1].add(fields['id'])
    rows = []
    for predicate, (test, tests) in skips.items():
        observed = f'boot {boot}: {len(tests)} test{"s" * (len(tests) != 1)}, first {test}'
        rows.append(row(f'hwtest.skipped-calls.{predicate}', OWNER, 'report', INFO, observed))
    return rows


def rolled_up(results: list[Result]) -> list[Result]:
    """Return `results` with HW Test's red and skip lines replaced by their roll-up rows.

    The rows read the newest boot with any HW Test line, so a clean boot after a red one rolls up clean.
    """
    lines = [f for r in results if r.check.subsystem == SUBSYSTEM for f in r.matches]
    boot = max(lines, key=lambda f: f['time'])['boot'] if lines else ''
    rows = []
    for result in results:
        if result.check.name == REDS:
            rows += red_rows(result.matches, boot)
        elif result.check.name == SKIPS:
            rows += skip_rows(result.matches, boot)
        else:
            rows.append(result)
    return rows


def headline(groups: list[tuple[str, list[Result]]]) -> str:
    """Return HW Test's first red from a failed command buffer and each predicate's reds, for the table's top."""
    rows = [
        r
        for _, results in groups
        for r in results
        if r.check.name == 'hwtest.first-red' or r.check.name.startswith('hwtest.red.')
    ]
    parts = [f'{r.check.name.removeprefix("hwtest.")}: {r.observed}' for r in rows]
    return 'HW Test reds from failed command buffers: ' + ('; '.join(parts) or 'no HW Test lines') + '\n'


def blocking(groups: list[tuple[str, list[Result]]]) -> str:
    """Return the failed checks that keep the build from counting as good, for the table's top."""
    failed = [r for _, results in groups for r in results if r.check.blocks and r.verdict == FAIL]
    parts = [f'{r.check.name}: {r.observed}' for r in failed]
    return 'Blocks the build: ' + ('; '.join(parts) or 'nothing') + '\n'


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


# A raw Metal object's selector that no Metal protocol declares and only the old front end answered: it raises now
SWITCH_METAL_SELECTOR = re.compile(r'-\[(SwitchMetal\w+) ([^\]]+)\]: unrecognized selector sent to')


def ips_body(path: Path) -> dict:
    """Return an .ips file's second JSON part ({} where it has none)."""
    with path.open(errors='replace') as f:
        f.readline()
        try:
            body = json.loads(f.read())
        except json.JSONDecodeError:
            return {}
    return body if isinstance(body, dict) else {}


def raised_selector(body: dict) -> str:
    """Return the '-[SwitchMetalClass selector]' an uncaught unrecognized-selector exception names, '' for none."""
    asi = ' '.join(' '.join(lines) for lines in (body.get('asi') or {}).values())
    m = SWITCH_METAL_SELECTOR.search(asi)
    return f'-[{m[1]} {m[2]}]' if m else ''


def loads_nvk(body: dict) -> bool:
    """Whether a report's image list holds NVK's Vulkan driver."""
    return any('libvulkan_nouveau' in (i.get('name', '') + i.get('path', '')) for i in body.get('usedImages') or [])


def crash_rows(directory: str | None) -> list[Result]:
    """Return the crash budget rows: crashes and panics fail, jetsam and the rest are counted per process."""
    owner = 'main'
    if not directory:
        return [row('crash.reports', owner, 'count == 0', NO_DATA, 'no --crashes directory')]
    kinds: dict[str, Counter] = {}
    stale: list[Path] = []
    raised: list[str] = []
    nvk: list[str] = []
    for path in sorted(Path(directory).rglob('*.ips')):
        bug_type, process = ips_header(path)
        kinds.setdefault(bug_type, Counter())[process] += 1
        if bug_type == CRASH and stale_sync_waiter(path):
            stale.append(path)
        body = ips_body(path)
        if bug_type == CRASH and raised_selector(body):
            raised.append(f'{process} {raised_selector(body)}')
        if loads_nvk(body):
            nvk.append(process)

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
    rows.append(
        row(
            'crash.gm20b-selector',
            'm2v-cut3',
            'count == 0',
            FAIL if raised else PASS,
            f'{len(raised)}: ' + ', '.join(raised[:6]),
        )
    )
    rows.append(
        row('crash.nvk-image', 'm2v-cut1', 'count == 0', FAIL if nvk else PASS, f'{len(nvk)}: ' + ', '.join(nvk[:6]))
    )
    rows.append(row('crash.jetsam', owner, 'report', INFO, listed(JETSAM)))
    other = {k: v for k, v in kinds.items() if k not in {CRASH, PANIC, JETSAM}}
    observed = '; '.join(f'type {k}: {sum(v.values())}' for k, v in sorted(other.items())) or 'none'
    rows.append(row('crash.other-reports', owner, 'report', INFO, observed))
    return rows
