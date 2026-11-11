"""The latest HW Test run against the one before it and the best each test and number has done: one screen of text."""

from dataclasses import dataclass, field

from tools.hwtest import metrics, posters
from tools.hwtest.reports import Report

SHOWN = 10
DETAIL = 220


@dataclass
class Changes:
    """What moved between two runs of the same app on the same iOS build."""

    regressions: list[str] = field(default_factory=list)
    new_failures: list[str] = field(default_factory=list)
    fixes: list[str] = field(default_factory=list)
    still_failing: list[str] = field(default_factory=list)
    missing: list[str] = field(default_factory=list)
    slower: list[str] = field(default_factory=list)
    faster: list[str] = field(default_factory=list)
    below_best: list[str] = field(default_factory=list)
    not_comparable: list[str] = field(default_factory=list)


def detail(entry: dict) -> str:
    """Return a failure's why= and its key=value fields, cut to fit a line."""
    fields = entry.get('log', '').split(' ', 3)[3:] or ['']
    text = fields[0]
    why = entry.get('detail', '')
    if why and 'why=' in text:
        text = text[: text.index('why=')].rstrip()
    line = f'{entry["id"]}: {why}  [{text}]' if why else f'{entry["id"]}: [{text}]'
    return line if len(line) <= DETAIL else line[: DETAIL - 1] + '…'


def last_pass(test: str, earlier: list[Report]) -> Report | None:
    return next((r for r in reversed(earlier) if r.results.get(test, {}).get('verdict') == 'PASS'), None)


def verdicts(latest: Report, previous: Report, earlier: list[Report], changes: Changes) -> None:
    before = previous.results
    for test, entry in latest.results.items():
        now, then = entry['verdict'], before.get(test, {}).get('verdict')
        if metrics.nak_change(entry, (latest.identity, previous.identity)):
            continue
        if now == 'FAIL' and then == 'PASS':
            changes.regressions.append(detail(entry))
        elif now == 'FAIL' and then != 'FAIL':
            changes.new_failures.append(detail(entry))
        elif now == 'PASS' and then == 'FAIL':
            changes.fixes.append(test)
        elif now == 'FAIL' and (passed := last_pass(test, earlier)):
            changes.still_failing.append(f'{test} (passed {passed.label()})')
    if latest.finished:
        planned = set(latest.run.get('order', []))
        changes.missing += [test for test in before if test not in latest.results and test in planned]


def numbers(latest: Report, previous: Report, earlier: list[Report], changes: Changes) -> None:
    for test, entry in latest.results.items():
        other = previous.results.get(test)
        if not other:
            continue
        reason = metrics.not_comparable(entry, other, (latest.identity, previous.identity))
        if reason:
            moved = f' ({other["verdict"]} -> {entry["verdict"]})' if other['verdict'] != entry['verdict'] else ''
            changes.not_comparable.append(f'{test}: {reason}{moved}')
            continue
        for key, value in entry.get('metrics', {}).items():
            way, now, then = metrics.direction(key), metrics.number(value), metrics.number(other['metrics'].get(key))
            if way and now is not None and then is not None:
                values = [metrics.number(r.results.get(test, {}).get('metrics', {}).get(key)) for r in earlier]
                measure(f'{test} {key}', way, (now, then), [v for v in values if v is not None][-10:], changes)


def measure(name: str, way: int, now_then: tuple[float, float], history: list[float], changes: Changes) -> None:
    """One metric against the previous run's value and the best of the earlier runs."""
    now, then = now_then
    if abs(now - then) > metrics.band(then, history):
        change = f' ({(now - then) / then * 100:+.0f}%)' if then else ''
        (changes.faster if (now - then) * way > 0 else changes.slower).append(f'{name}: {then:g} -> {now:g}{change}')
    best = max(history, key=lambda v: v * way, default=then)
    if (best - then) * way > 0 and (best - now) * way > metrics.band(best, history):
        changes.below_best.append(f'{name}: {now:g}, best {best:g}')


def compare(latest: Report, earlier: list[Report]) -> Changes:
    """`earlier`: the same key's reports before `latest`, oldest first."""
    changes = Changes()
    if earlier:
        verdicts(latest, earlier[-1], earlier[:-1], changes)
        numbers(latest, earlier[-1], earlier[:-1], changes)
    return changes


def stopped(latest: Report) -> list[str]:
    """Why an unfinished run stopped: the test that was running and what the boot's reports say."""
    if latest.run.get('status') == 'cancelled':
        return ['CANCELLED: the run was cancelled; completed results were kept']
    if latest.finished:
        return []
    health = latest.mac.get('health') or {}
    interruption = latest.run.get('interruption')
    if interruption:
        return [f'INTERRUPTED at {latest.current or "(between tests)"}: the app was {interruption}, not a crash']
    lines = [f'STOPPED at {latest.current or "(between tests)"}: the run never finished']
    lines += [f'  HW Test crash report: {crash}' for crash in health.get('hwtest_crashes', [])]
    lines += [f'  kernel panic saved: {panic}' for panic in health.get('panics', [])]
    if len(lines) == 1:
        lines.append('  no HW Test crash report or saved panic for it: a hang, a watchdog reset or power loss')
    return lines


def section(title: str, items: list[str]) -> list[str]:
    if not items:
        return []
    shown = [f'  {item}' for item in items[:SHOWN]]
    more = [f'  +{len(items) - SHOWN} more'] if len(items) > SHOWN else []
    return [f'{title} ({len(items)})', *shown, *more]


def health_lines(latest: Report, previous: Report | None) -> list[str]:
    health = latest.mac.get('health')
    if not health:
        return []
    before = (previous.mac.get('health') or {}) if previous else {}

    def counted(key: str) -> str:
        return ', '.join(f'{name} {count}' for name, count in sorted(health.get(key, {}).items())) or 'none'

    boot = health.get('boot_s')
    was = before.get('boot_s')
    crashes = f'this boot: crashes {counted("crashes")}; CADisplay aborts {counted("cadisplay_aborts")}'
    lines = [f'{crashes}; jetsam events {health.get("jetsams", 0)}; paired hosts {health.get("pair_records", 0)}']
    apps = health.get('apps')
    if apps:
        counts = '{installed} installed, {placeholders} placeholders, {system_apps_wanted} system apps wanted'
        lines.append('apps: ' + counts.format(**apps))
    if boot is not None:
        lines.append(f'boot to SpringBoard {boot:g} s' + (f" (last run's boot {was:g} s)" if was is not None else ''))
    poster = posters.summary_line(health.get('lock_poster'))
    if poster:
        lines.append(poster)
    return lines


def summary(latest: Report, earlier: list[Report]) -> str:
    counts = latest.data.get('summary', {})
    head = [
        f'HW Test {latest.identity.get("bundle_id", "?")} {latest.label()}',
        '  {pass} pass, {fail} fail, {skip} skip, {crashed} crashed of {planned} planned'.format(
            **{k: counts.get(k, 0) for k in ('pass', 'fail', 'skip', 'crashed', 'planned')}
        ),
    ]
    against = [f'  against {earlier[-1].label()}'] if earlier else ['  no earlier run of this app on this build']
    changes = compare(latest, earlier)
    lines = head + against + stopped(latest) + health_lines(latest, earlier[-1] if earlier else None)
    lines += section('REGRESSIONS (passed last run)', changes.regressions)
    lines += section('NEW FAILURES', changes.new_failures)
    lines += section('FIXED', changes.fixes)
    lines += section('still failing, passed before', changes.still_failing)
    lines += section('not run this time', changes.missing)
    lines += section('slower', changes.slower)
    lines += section('faster', changes.faster)
    lines += section('below the best run', changes.below_best)
    lines += section('numbers not comparable', changes.not_comparable)
    return '\n'.join(lines)
