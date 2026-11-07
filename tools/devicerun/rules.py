"""A device check's declaration, the log events it matches, and its verdict."""

import re
from collections import Counter
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import PurePosixPath

PASS, FAIL, NO_DATA, INFO = 'PASS', 'FAIL', 'NO-DATA', 'INFO'

# count | rate (per minute over the log's span) | boots (with a match) | agg(field) or agg(a/b*k): then OP number.
RULE = re.compile(
    r'^(?:(?P<metric>count|rate|boots)|(?P<agg>sum|max|min|last|p50|p99)\((?P<expr>[\w/*]+)\))'
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
REPORT, TIMELINE = 'report', 'timeline'
LAUNCHD_PID = re.compile(r' \[\d+\]$')
TOP_GROUPS = 6
TIMELINE_ROWS = 40


@dataclass(frozen=True)
class Check:
    """One expectation about the device log.

    `contains` (one literal or alternatives), `process`, `subsystem` (substring) and `level` become the `log show`
    predicate; `regex` (searched) narrows further and its named groups are the fields `rule`, `where` and `show` use.
    `needs` names a check that must have matched for this one to mean anything (a failure line of a feature that is
    not running is NO-DATA, not PASS). `against` names a check whose `key` values this one's are looked up in.
    `boot` limits the check to one of the morning's boot labels (A, B tiled cache, K kprof).
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
    needs: str = ''
    against: str = ''
    key: str = ''
    source: str = ''
    note: str = ''
    boot: str = ''

    def literals(self) -> tuple[str, ...]:
        return (self.contains,) if isinstance(self.contains, str) else self.contains


@dataclass
class Event:
    time: datetime
    boot: str
    process: str
    subsystem: str
    category: str
    level: str
    message: str

    @classmethod
    def from_ndjson(cls, record: dict) -> 'Event':
        return cls(
            time=datetime.strptime(record['timestamp'], '%Y-%m-%d %H:%M:%S.%f%z'),
            boot=record.get('bootUUID', ''),
            process=PurePosixPath(record.get('processImagePath') or '').name,
            subsystem=record.get('subsystem') or '',
            category=record.get('category') or '',
            level=record.get('messageType') or '',
            message=record.get('eventMessage') or '',
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
    if m['metric'] == 'rate':
        rate = len(matches) / minutes if minutes else 0.0
        return rate, f'{rate:.2f}/min over {minutes:.1f} min ({len(matches)} lines)'
    xs = values(matches, m['expr'])
    if not xs:
        return None, f'{len(matches)} lines, no {m["expr"]}'
    got = aggregate(m['agg'], xs)
    return got, f'{m["agg"]}({m["expr"]}) {fmt(got)} over {len(xs)} lines'


def evaluate(check: Check, matches: list[dict], minutes: float, by_name: dict[str, list[dict]]) -> Result:
    """Return the check's verdict over the events it matched."""
    matches = filtered(check, matches)
    if check.needs and not by_name.get(check.needs):
        return Result(check, NO_DATA, f'no {check.needs} line', matches)
    if check.rule == TIMELINE:
        return Result(check, INFO if matches else NO_DATA, timeline(matches) or 'no lines', matches)
    if check.rule == REPORT:
        observed = describe(check, matches, minutes)
        if check.against:
            observed += '; ' + lookup(check, matches, by_name.get(check.against, []))
        return Result(check, INFO if matches else NO_DATA, observed, matches)
    got, observed = measure(check, matches, minutes)
    if got is None:
        return Result(check, NO_DATA, observed, matches)
    m = RULE.match(check.rule)
    verdict = PASS if OPS[m['op']](got, float(m['value'])) else FAIL
    return Result(check, verdict, observed, matches)
