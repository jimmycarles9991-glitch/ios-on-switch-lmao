"""A device check's declaration, the log events it matches, and its verdict."""

import re
from collections import Counter
from dataclasses import dataclass, field
from datetime import datetime, timedelta
from pathlib import PurePosixPath

from tools.devicerun import renamed

PASS, FAIL, NO_DATA, INFO, NA = 'PASS', 'FAIL', 'NO-DATA', 'INFO', 'N/A'
# Works, but not the way it should: the usb group's keyboard through the kernel HID driver after its dext failed.
FALLBACK = 'FALLBACK'

# count | rate (per minute over the log's span) | boots (with a match) | repeats (lines past the first of a boot with
# the same `show` fields) | agg(field) or agg(a/b*k): then OP number.
RULE = re.compile(
    r'^(?:(?P<metric>count|rate|boots|repeats)|(?P<agg>sum|max|min|last|p50|p99)\((?P<expr>[\w/*]+)\))'
    r'\s*(?P<op>==|!=|<=|>=|<|>)\s*(?P<value>-?[\d.]+)$'
)
WHERE = re.compile(r'^(?P<field>\w+)\s*(?P<op>==|!=|<=|>=|<|>)\s*(?P<value>-?[\d.]+)$')
EXPR = re.compile(r'^(?P<a>\w+)(?:/(?P<b>\w+))?(?:\*(?P<k>[\d.]+))?$')
OPS = {
    '==': lambda a, b: a == b,
    '!=': lambda a, b: a != b,
    '<=': lambda a, b: a <= b,
    '>=': lambda a, b: a >= b,
    '<': lambda a, b: a < b,
    '>': lambda a, b: a > b,
}
# sums: an INFO row of each `show` field's total over the lines, for counters a line carries per window.
REPORT, TIMELINE, SUMS = 'report', 'timeline', 'sums'
DEBUG_LEVELS = {'info', 'debug'}
LAUNCHD_PID = re.compile(r' \[\d+\]$')
TOP_GROUPS = 6
TIMELINE_ROWS = 40


@dataclass(frozen=True)
class Check:
    """One expectation about the device log.

    `contains` (one literal or alternatives), `process`, `subsystem` (substring) and `level` become the `log show`
    predicate; `regex` (searched) narrows further and its named groups are the fields `rule`, `where` and `show` use.
    `debug` lets the check see info and debug lines (the run then asks `log show` for them); every other check sees
    default, error and fault lines only. `needs` names a check, or several, that must have matched for this one to mean
    anything (a failure line of a feature that is not running is NO-DATA, not PASS). `applies` ('<check>
    <field>=<value>') limits the check to logs where that check matched with that field (an iOS 27 app's test where the
    suite line says ios=27): in any other log it is N/A, never PASS. `against` names a check whose `key` values this
    one's are looked up in. `followed` names a check one of whose lines must come within `within` seconds of each of
    this check's (with the same `key` value, when `key` is set), counted from the line's time less its `since` field in
    milliseconds: the rule then counts the lines without one (with `pending_at_end`, a line whose window the log's end
    cuts is pending, never missed). `after` keeps only the lines that come within `within` seconds after one of that
    check's lines, and no sooner than `after_min` seconds when set. `boot` limits the check to one of the morning's boot
    labels (A, B tiled cache, K kprof). `unless` names a check whose lines excuse this one's in the same boot, or only
    within `unless_within` seconds of one of them when set (with the same `key` value, when `key` is set). `outside` (an
    opening check, a closing one) drops the lines that come while a span is open: after one of the opening check's lines
    with none of the closing one's since, in the same boot. `failure` leads the observed text of a FAIL. `during` names
    a check whose lines are tests that kill this check's subjects on purpose, each test's window its line's time less
    its `ms` field up to its time plus `within` seconds: this check's lines in a window of the same boot that match
    `excusable` (searched; every line when empty) are that test's, listed as expected ("killed by <its id>") and left
    out of the rule. A `process` of '!<name>' matches every process whose name does not begin with <name>. `per` names a
    check whose lines are tests' runs, each a window as `during`'s: the verdict is then each window's count of this
    check's lines in the same boot against `per_count`, that line's field or a number (FAIL where any differs, NO_DATA
    with no run), and `rule` is not read. `preceded` names a check: only this check's lines that one of its lines came
    within `within` seconds before, in the same boot (with the same `key` value, when `key` is set), are judged.
    `boots` names a check: only this check's lines in a boot where one of its lines came are judged (an iPadOS 27
    boot's, in an archive of several). `blocks`: a FAIL of this check keeps the build from counting as good, and the
    table's top names it.
    """

    name: str
    owner: str
    contains: str | tuple[str, ...]
    rule: str = 'count == 0'
    process: str = ''
    subsystem: str = ''
    level: str = ''
    regex: str = ''
    where: str = ''
    show: tuple[str, ...] = ()
    needs: str | tuple[str, ...] = ''
    applies: str = ''
    against: str = ''
    key: str = ''
    followed: str = ''
    within: float = 0.0
    since: str = ''
    after: str = ''
    after_min: float = 0.0
    source: str = ''
    note: str = ''
    boot: str = ''
    unless: str = ''
    unless_within: float = 0.0
    pending_at_end: bool = False
    debug: bool = False
    outside: tuple[str, str] = ()
    failure: str = ''
    during: str = ''
    excusable: str = ''
    per: str = ''
    per_count: str = ''
    preceded: str = ''
    boots: str = ''
    blocks: bool = False

    def literals(self) -> tuple[str, ...]:
        return (self.contains,) if isinstance(self.contains, str) else self.contains

    def needed(self) -> tuple[str, ...]:
        return (self.needs,) if isinstance(self.needs, str) and self.needs else tuple(self.needs or ())


@dataclass
class Event:
    time: datetime
    boot: str
    process: str
    subsystem: str
    category: str
    level: str
    message: str
    mach: int = 0

    @classmethod
    def from_ndjson(cls, record: dict) -> 'Event':
        return cls(
            time=datetime.strptime(record['timestamp'], '%Y-%m-%d %H:%M:%S.%f%z'),
            boot=record.get('bootUUID', ''),
            process=PurePosixPath(record.get('processImagePath') or '').name,
            subsystem=record.get('subsystem') or '',
            category=record.get('category') or '',
            level=record.get('messageType') or '',
            message=renamed.current(record.get('eventMessage') or ''),
            mach=int(record.get('machTimestamp') or 0),
        )


@dataclass
class Result:
    check: Check
    verdict: str
    observed: str
    matches: list[dict] = field(default_factory=list)


def process_matches(check: Check, process: str) -> bool:
    if not check.process:
        return True
    if check.process.startswith('!'):
        return not process.startswith(check.process[1:])
    if check.process == 'kernel':
        return process.startswith('kernel')
    return process == check.process


def match(check: Check, event: Event, regex: re.Pattern | None) -> dict | None:
    """Return the event's fields for this check, or None when it does not match."""
    if not any(literal in event.message for literal in check.literals()):
        return None
    if not process_matches(check, event.process) or check.subsystem not in event.subsystem:
        return None
    if check.level and event.level.lower() != check.level.lower():
        return None
    if not check.debug and event.level.lower() in DEBUG_LEVELS:
        return None
    groups = {}
    if regex:
        m = regex.search(event.message)
        if not m:
            return None
        groups = {k: v for k, v in m.groupdict().items() if v is not None}
    return {
        'time': event.time,
        'boot': event.boot[:8],
        'process': event.process,
        'category': event.category,
        'job': LAUNCHD_PID.sub('', event.subsystem),
        'message': event.message,
        'mach': event.mach,
        **groups,
    }


def number(text: str) -> float:
    return float(int(text, 16)) if text.startswith('0x') else float(text)


def values(matches: list[dict], expr: str) -> list[float]:
    m = EXPR.match(expr)
    if not m:
        raise ValueError(f'bad field expression {expr!r}')
    out = []
    for fields in matches:
        if m['a'] not in fields or (m['b'] and m['b'] not in fields):
            continue
        v = number(fields[m['a']])
        if m['b']:
            d = number(fields[m['b']])
            if not d:
                continue
            v /= d
        out.append(v * float(m['k'] or 1))
    return out


def aggregate(agg: str, xs: list[float]) -> float:
    if agg == 'sum':
        return sum(xs)
    if agg == 'max':
        return max(xs)
    if agg == 'min':
        return min(xs)
    if agg == 'last':
        return xs[-1]
    ordered = sorted(xs)
    q = 0.5 if agg == 'p50' else 0.99
    return ordered[min(len(ordered) - 1, int(q * len(ordered)))]


def filtered(check: Check, matches: list[dict]) -> list[dict]:
    if not check.where:
        return matches
    m = WHERE.match(check.where)
    if not m:
        raise ValueError(f'{check.name}: bad where {check.where!r}')
    want = float(m['value'])
    return [f for f in matches if m['field'] in f and OPS[m['op']](number(f[m['field']]), want)]


def clock(time: datetime) -> str:
    return f'{time:%H:%M:%S}.{time.microsecond // 1000:03d}'


def fmt(x: float) -> str:
    return str(int(x)) if float(x).is_integer() else f'{x:.2f}'


def grouped(check: Check, matches: list[dict], minutes: float) -> str:
    counts = Counter(tuple(str(f.get(k, '?'))[:40] for k in check.show) for f in matches)
    parts = []
    for key, n in counts.most_common(TOP_GROUPS):
        rate = f' ({n / minutes:.1f}/min)' if minutes else ''
        parts.append(f'{"/".join(key)} x{n}{rate}')
    more = len(counts) - TOP_GROUPS
    return '; '.join(parts) + (f'; +{more} more' if more > 0 else '')


def timeline(matches: list[dict]) -> str:
    rows = [f'{f["time"]:%H:%M:%S} {f["message"]}' for f in matches[:TIMELINE_ROWS]]
    more = len(matches) - TIMELINE_ROWS
    return ' | '.join(rows) + (f' | +{more} more' if more > 0 else '')


def lookup(check: Check, matches: list[dict], others: list[dict]) -> str:
    keys = {f[check.key] for f in matches if check.key in f}
    seen = {f[check.key] for f in others if check.key in f}
    missing = sorted(keys - seen)
    listed = ' '.join(missing[:12]) + (' ...' if len(missing) > 12 else '')
    return f'{len(keys)} {check.key}s, {len(missing)} without a {check.against} line' + (
        f': {listed}' if missing else ''
    )


def unfollowed(
    check: Check, matches: list[dict], followers: list[dict], log_end: datetime | None = None
) -> tuple[list[dict], list[dict]]:
    """Return the matches no `followed` line came within `within` seconds of, and those the log's end cut short."""
    missed, pending = [], []
    for fields in matches:
        start = fields['time']
        if check.since in fields:
            start -= timedelta(milliseconds=number(fields[check.since]))
        end = start + timedelta(seconds=check.within)
        if not any(
            start <= f['time'] <= end and (not check.key or f.get(check.key) == fields.get(check.key))
            for f in followers
        ):
            (pending if log_end is not None and end > log_end else missed).append(fields)
    return missed, pending


def follow(
    check: Check, matches: list[dict], minutes: float, by_name: dict[str, list[dict]], log_end: datetime | None = None
) -> Result:
    """Return the verdict of a `followed` check: its rule over the lines nothing followed."""
    if not matches:
        return Result(check, NO_DATA, 'no lines', matches)
    end = log_end if check.pending_at_end else None
    missed, pending = unfollowed(check, matches, by_name.get(check.followed, []), end)
    cut = f'{len(pending)} with the log ending inside their {fmt(check.within)} s' if pending else ''
    if len(pending) == len(matches):
        return Result(check, NO_DATA, f'{len(matches)} lines, {cut}', pending)
    got, _ = measure(check, missed, minutes)
    m = RULE.match(check.rule)
    verdict = PASS if OPS[m['op']](got, float(m['value'])) else FAIL
    listed = ' '.join(f'{f["time"]:%H:%M:%S}' + (f' {f[check.key]}' if check.key in f else '') for f in missed[:8])
    observed = f'{len(matches)} lines, {len(missed)} without a {check.followed} line within {fmt(check.within)} s'
    observed += (f': {listed}' if missed else '') + (f'; {cut}' if cut else '')
    return Result(check, verdict, failed_text(check, verdict, observed), missed)


def failed_text(check: Check, verdict: str, observed: str) -> str:
    return f'{check.failure}: {observed}' if verdict == FAIL and check.failure else observed


def excused(check: Check, matches: list[dict], by_name: dict[str, list[dict]]) -> list[dict]:
    """Return the lines `unless` does not excuse: none in its boots, or only those near its lines (of the same key)."""
    excusers = by_name.get(check.unless, [])
    if not check.unless_within:
        boots = {fields['boot'] for fields in excusers}
        return [fields for fields in matches if fields['boot'] not in boots]
    window = timedelta(seconds=check.unless_within)
    return [
        fields
        for fields in matches
        if not any(
            e['boot'] == fields['boot']
            and abs(e['time'] - fields['time']) <= window
            and (not check.key or e.get(check.key) == fields.get(check.key))
            for e in excusers
        )
    ]


def span_opener(fields: dict, opens: list[dict], closes: list[dict]) -> dict | None:
    """Return the line that opened the span this line falls in (the last opening line, none closing it), if any."""
    before = [(m['time'], 0, m) for m in opens if m['boot'] == fields['boot'] and m['time'] <= fields['time']]
    before += [(m['time'], 1, m) for m in closes if m['boot'] == fields['boot'] and m['time'] <= fields['time']]
    if not before:
        return None
    _, closing, m = max(before, key=lambda b: b[:2])
    return None if closing else m


def after(check: Check, matches: list[dict], by_name: dict[str, list[dict]]) -> list[dict]:
    """Return the lines that come between `after_min` and `within` seconds after one of the `after` check's lines."""
    earliest, window = timedelta(seconds=check.after_min), timedelta(seconds=check.within)
    anchors = by_name.get(check.after, [])
    return [
        fields
        for fields in matches
        if any(a['boot'] == fields['boot'] and earliest <= fields['time'] - a['time'] <= window for a in anchors)
    ]


def describe(check: Check, matches: list[dict], minutes: float) -> str:
    boots = len({f['boot'] for f in matches})
    parts = [f'{len(matches)} lines' + (f' in {boots} boots' if boots > 1 else '')]
    if check.show and matches:
        parts.append(grouped(check, matches, minutes))
    return '; '.join(parts)


def measure(check: Check, matches: list[dict], minutes: float) -> tuple[float | None, str]:
    """Return the rule's metric (None: nothing to measure) and how it reads."""
    m = RULE.match(check.rule)
    if not m:
        raise ValueError(f'{check.name}: bad rule {check.rule!r}')
    if m['metric'] == 'count':
        return len(matches), describe(check, matches, minutes)
    if m['metric'] == 'boots':
        return len({f['boot'] for f in matches}), describe(check, matches, minutes)
    if m['metric'] == 'repeats':
        seen = Counter((f['boot'], *(str(f.get(k, '?')) for k in check.show)) for f in matches)
        return sum(n - 1 for n in seen.values()), describe(check, matches, minutes)
    if m['metric'] == 'rate':
        rate = len(matches) / minutes if minutes else 0.0
        return rate, f'{rate:.2f}/min over {minutes:.1f} min ({len(matches)} lines)'
    xs = values(matches, m['expr'])
    if not xs:
        return None, f'{len(matches)} lines, no {m["expr"]}'
    got = aggregate(m['agg'], xs)
    return got, f'{m["agg"]}({m["expr"]}) {fmt(got)} over {len(xs)} lines'


def applicable(applies: str, by_name: dict[str, list[dict]]) -> bool:
    """Whether a match of the check `applies` names has its field at its value ('<check> <field>=<value>')."""
    name, _, condition = applies.partition(' ')
    key, _, value = condition.partition('=')
    return any(fields.get(key) == value for fields in by_name.get(name, []))


def informational(check: Check, matches: list[dict], minutes: float, by_name: dict[str, list[dict]]) -> Result:
    """Return a 'timeline' or 'report' check's INFO row."""
    if check.rule == TIMELINE:
        return Result(check, INFO if matches else NO_DATA, timeline(matches) or 'no lines', matches)
    if check.rule == SUMS:
        totals = ', '.join(f'{k} {fmt(sum(values(matches, k)))}' for k in check.show)
        return Result(check, INFO if matches else NO_DATA, f'{len(matches)} lines; {totals}', matches)
    observed = describe(check, matches, minutes)
    if check.against:
        observed += '; ' + lookup(check, matches, by_name.get(check.against, []))
    return Result(check, INFO if matches else NO_DATA, observed, matches)


def led(check: Check, matches: list[dict], leaders: list[dict]) -> list[dict]:
    """Return the matches a `preceded` line came within `within` seconds before, in the same boot."""
    window = timedelta(seconds=check.within)
    return [
        fields
        for fields in matches
        if any(
            f['boot'] == fields['boot']
            and fields['time'] - window <= f['time'] <= fields['time']
            and (not check.key or f.get(check.key) == fields.get(check.key))
            for f in leaders
        )
    ]


def killed_during(check: Check, matches: list[dict], tests: list[dict]) -> tuple[list[dict], list[dict]]:
    """Split `matches` into those `check.during`'s tests do not account for and those they do (each with `killer`)."""
    slack = timedelta(seconds=check.within)
    windows = [
        (
            t['boot'],
            t['time'] - timedelta(milliseconds=number(t['ms'])),
            t['time'] + slack,
            t.get('id', check.during),
        )
        for t in tests
        if 'ms' in t
    ]
    excusable = re.compile(check.excusable) if check.excusable else None
    kept, excused_matches = [], []
    for fields in matches:
        killer = next((i for boot, a, b, i in windows if boot == fields['boot'] and a <= fields['time'] <= b), None)
        if killer and (not excusable or excusable.search(fields['message'])):
            excused_matches.append({**fields, 'killer': killer})
        else:
            kept.append(fields)
    return kept, excused_matches


def expected_kills(check: Check, excused_matches: list[dict]) -> str:
    by_test: dict[str, list[dict]] = {}
    for fields in excused_matches:
        by_test.setdefault(fields['killer'], []).append(fields)
    return '; '.join(
        f'expected: killed by {test}: ' + (grouped(check, lines, 0) if check.show else f'{len(lines)} lines')
        for test, lines in by_test.items()
    )


def evaluate(
    check: Check,
    matches: list[dict],
    minutes: float,
    by_name: dict[str, list[dict]],
    log_end: datetime | None = None,
) -> Result:
    """Return the check's verdict over the events it matched (`log_end`: the last line's time, for `followed`)."""
    matches = filtered(check, matches)
    if check.boots:
        boots = {fields['boot'] for fields in by_name.get(check.boots, [])}
        matches = [fields for fields in matches if fields['boot'] in boots]
    if check.unless:
        matches = excused(check, matches, by_name)
    if check.after:
        matches = after(check, matches, by_name)
    if check.outside:
        opens, closes = (by_name.get(name, []) for name in check.outside)
        matches = [fields for fields in matches if span_opener(fields, opens, closes) is None]
    if check.preceded:
        matches = led(check, matches, by_name.get(check.preceded, []))
    killed: list[dict] = []
    if check.during:
        matches, killed = killed_during(check, matches, by_name.get(check.during, []))
    result = judge(check, matches, minutes, by_name, log_end)
    if killed:
        result.observed += '; ' + expected_kills(check, killed)
    return result


def judge(
    check: Check,
    matches: list[dict],
    minutes: float,
    by_name: dict[str, list[dict]],
    log_end: datetime | None = None,
) -> Result:
    """Return the check's verdict over the lines it judges."""
    missing = next((name for name in check.needed() if not by_name.get(name)), '')
    if missing:
        return Result(check, NO_DATA, f'no {missing} line', matches)
    if check.applies and not applicable(check.applies, by_name):
        return Result(check, NA, f'not run here: no {check.applies} line', matches)
    if check.followed or check.per:
        runs = by_name.get(check.per, [])
        return follow(check, matches, minutes, by_name, log_end) if check.followed else per_run(check, matches, runs)
    if check.rule in {TIMELINE, REPORT, SUMS}:
        return informational(check, matches, minutes, by_name)
    got, observed = measure(check, matches, minutes)
    if got is None:
        return Result(check, NO_DATA, observed, matches)
    m = RULE.match(check.rule)
    verdict = PASS if OPS[m['op']](got, float(m['value'])) else FAIL
    return Result(check, verdict, failed_text(check, verdict, observed), matches)


def per_run(check: Check, matches: list[dict], runs: list[dict]) -> Result:
    """Each run's count of `matches` in its window against `check.per_count` (its field, else the number itself)."""
    counted = []
    for run in runs:
        want = str(run.get(check.per_count, check.per_count))
        if 'ms' not in run or not want.isdigit():
            continue
        start = run['time'] - timedelta(milliseconds=number(run['ms']))
        got = sum(1 for f in matches if f['boot'] == run['boot'] and start <= f['time'] <= run['time'])
        counted.append((run['time'], got, int(want)))
    if not counted:
        return Result(check, NO_DATA, f'no {check.per} run', matches)
    off = [c for c in counted if c[1] != c[2]]
    rows = ' '.join(f'{t:%H:%M:%S} {got}/{want}' for t, got, want in (off or counted)[:8])
    return Result(
        check, FAIL if off else PASS, f'{len(counted)} runs, {len(off)} off (lines/expected): {rows}', matches
    )
