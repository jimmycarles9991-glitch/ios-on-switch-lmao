"""Every device test the agents wrote, run against a log archive pulled from the device: one PASS/FAIL table.

Pull the archive with `idevicesyslog archive <dir>.logarchive` (and the crash reports with `idevicecrashreport`), then
run this on it. One `log show` pulls every line any check could match; each check then counts its own lines.
"""

import argparse
import csv
import json
import re
import subprocess
import sys
from collections.abc import Iterable, Iterator

from tools.devicerun.checks import FRAME_PREFIX, GROUPS
from tools.devicerun.morning import CHECKLIST
from tools.devicerun.reports import crash_rows, frame_rows
from tools.devicerun.rules import FAIL, Check, Event, Result, evaluate, match

OBSERVED_WIDTH = 110


def quote(text: str) -> str:
    return '"' + text.replace('\\', '\\\\').replace('"', '\\"') + '"'


def clause(check: Check) -> str:
    literals = ' OR '.join(f'eventMessage CONTAINS {quote(lit)}' for lit in check.literals())
    parts = [f'({literals})']
    if check.process == 'kernel':
        parts.append('process BEGINSWITH "kernel"')
    elif check.process:
        parts.append(f'process == {quote(check.process)}')
    if check.subsystem:
        parts.append(f'subsystem CONTAINS {quote(check.subsystem)}')
    if check.level:
        parts.append(f'messageType == {check.level}')
    return '(' + ' AND '.join(parts) + ')'


def predicate(checks: Iterable[Check]) -> str:
    clauses = dict.fromkeys(clause(c) for c in checks)
    return ' OR '.join([*clauses, f'eventMessage BEGINSWITH {quote(FRAME_PREFIX)}'])


def log_show(archive: str, query: str, start: str | None, end: str | None) -> Iterator[dict]:
    """Yield the archive's matching records, as `log show --style ndjson` prints them."""
    cmd = ['/usr/bin/log', 'show', '--archive', archive, '--style', 'ndjson', '--predicate', query]
    if start:
        cmd += ['--start', start]
    if end:
        cmd += ['--end', end]
    with subprocess.Popen(cmd, stdout=subprocess.PIPE, text=True, errors='replace') as proc:
        for line in proc.stdout:
            record = json.loads(line)
            if record.get('eventType') == 'logEvent':
                yield record
    if proc.returncode:
        raise SystemExit(f'log show failed ({proc.returncode})')


def read_ndjson(path: str) -> Iterator[dict]:
    with open(path, errors='replace') as f:
        for line in f:
            record = json.loads(line)
            if record.get('eventType') == 'logEvent':
                yield record


def run_checks(records: Iterable[dict], checks: list[Check]) -> tuple[list[Result], list[str]]:
    """Return every check's result and the frame lines ("<timestamp> <message>") for tools/frames."""
    regexes = {c.name: re.compile(c.regex) if c.regex else None for c in checks}
    by_name: dict[str, list[dict]] = {c.name: [] for c in checks}
    frame_lines, first, last = [], None, None
    for record in records:
        event = Event.from_ndjson(record)
        first, last = first or event.time, event.time
        if event.message.startswith(FRAME_PREFIX):
            frame_lines.append(f'{record["timestamp"]} {event.message}')
        for check in checks:
            fields = match(check, event, regexes[check.name])
            if fields is not None:
                by_name[check.name].append(fields)
    minutes = (last - first).total_seconds() / 60 if first else 0.0
    return [evaluate(c, by_name[c.name], minutes, by_name) for c in checks], frame_lines


def expected(check: Check) -> str:
    return check.rule + (f' ({check.note})' if check.note else '')


def table(groups: list[tuple[str, list[Result]]]) -> str:
    rows = [('group', 'check', 'owner', 'verdict', 'expected', 'observed')]
    for group, results in groups:
        rows += [(group, r.check.name, r.check.owner, r.verdict, r.check.rule, r.observed) for r in results]
    widths = [max(len(row[i]) for row in rows) for i in range(5)]
    out = []
    for row in rows:
        cells = [cell.ljust(width) for cell, width in zip(row[:5], widths, strict=True)]
        observed = row[5] if len(row[5]) <= OBSERVED_WIDTH else row[5][: OBSERVED_WIDTH - 3] + '...'
        out.append('  '.join([*cells, observed]).rstrip())
    return '\n'.join(out)


def write_tsv(path: str, groups: list[tuple[str, list[Result]]]) -> None:
    with open(path, 'w', newline='') as f:
        writer = csv.writer(f, delimiter='\t', lineterminator='\n')
        writer.writerow(['group', 'check', 'owner', 'verdict', 'expected', 'observed', 'source'])
        for group, results in groups:
            for r in results:
                writer.writerow(
                    [group, r.check.name, r.check.owner, r.verdict, expected(r.check), r.observed, r.check.source]
                )


def selected(label: str | None) -> dict[str, list[Check]]:
    """Return the groups' checks that apply to this boot label (every check when there is none)."""
    return {name: [c for c in group if not label or c.boot in {'', label}] for name, group in GROUPS.items()}


def checklist(label: str | None) -> str:
    steps = CHECKLIST[''] + (CHECKLIST.get(label, []) if label else [])
    return '\n'.join([f'Boot {label or "(no label)"}: manual steps before pulling the logs', *steps, ''])


def evaluate_all(
    records: Iterable[dict], crashes: str | None, label: str | None = None
) -> list[tuple[str, list[Result]]]:
    groups_in = selected(label)
    checks = [c for group in groups_in.values() for c in group]
    results, frame_lines = run_checks(records, checks)
    by_check = {r.check.name: r for r in results}
    groups = [(name, [by_check[c.name] for c in group]) for name, group in groups_in.items()]
    groups.append(('frames', frame_rows(frame_lines)))
    groups.append(('crashes', crash_rows(crashes)))
    return groups


def main(argv: list[str] | None = None) -> int:
    """Print the table; exit 1 when any check fails."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('archive', help='a pulled .logarchive (absolute path under bazel run)')
    parser.add_argument('--crashes', help='a directory of pulled crash reports (.ips)')
    parser.add_argument('--tsv', help='also write the table, with every check source, as TSV here')
    parser.add_argument('--start', help="log show's --start, e.g. '2026-11-07 09:00:00'")
    parser.add_argument('--end', help="log show's --end")
    parser.add_argument('--boot-label', choices=['A', 'B', 'K'], help='the morning boot: A, B tiled cache, K kprof')
    parser.add_argument('--ndjson', action='store_true', help='the input is `log show --style ndjson` output')
    parser.add_argument('--predicate', action='store_true', help='print the log show predicate and exit')
    args = parser.parse_args(argv)
    checks = [c for group in selected(args.boot_label).values() for c in group]
    if args.predicate:
        print(predicate(checks))
        return 0
    if args.ndjson:
        records = read_ndjson(args.archive)
    else:
        records = log_show(args.archive, predicate(checks), args.start, args.end)
    print(checklist(args.boot_label))
    groups = evaluate_all(records, args.crashes, args.boot_label)
    print(table(groups))
    if args.tsv:
        write_tsv(args.tsv, groups)
    verdicts = [r.verdict for _, results in groups for r in results]
    print(' '.join(f'{v} {verdicts.count(v)}' for v in sorted(set(verdicts))))
    return 1 if FAIL in verdicts else 0


if __name__ == '__main__':
    sys.exit(main())
