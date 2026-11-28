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

from tools.devicerun import renamed, usb
from tools.devicerun.checks import FRAME_PREFIX, GROUPS
from tools.devicerun.commit import commit_rows
from tools.devicerun.cpu import cpu_rows
from tools.devicerun.hwtest import joined
from tools.devicerun.launch import launch_rows
from tools.devicerun.memory import memory_rows
from tools.devicerun.pack_image import pack_image_rows
from tools.devicerun.morning import CHECKLIST
from tools.devicerun.owed_all import summary as owed_summary
from tools.devicerun.reports import blocking, crash_rows, frame_rows, headline, rolled_up
from tools.devicerun.rules import FAIL, INFO, REPORT, Check, Event, Result, evaluate, match
from tools.devicerun.shared_cache import cache_rows
from tools.devicerun.springboard import springboard_rows

OBSERVED_WIDTH = 110
KERNEL_LINE = 'Darwin Kernel Version '


def quote(text: str) -> str:
    return '"' + text.replace('\\', '\\\\').replace('"', '\\"') + '"'


def clause(check: Check) -> str:
    literals = ' OR '.join(
        f'eventMessage CONTAINS {quote(spelled)}' for lit in check.literals() for spelled in renamed.spellings(lit)
    )
    parts = [f'({literals})']
    if check.process == 'kernel':
        parts.append('process BEGINSWITH "kernel"')
    elif check.process.startswith('!'):
        parts.append(f'NOT (process BEGINSWITH {quote(check.process[1:])})')
    elif check.process:
        parts.append(f'process == {quote(check.process)}')
    if check.subsystem:
        parts.append(f'subsystem CONTAINS {quote(check.subsystem)}')
    if check.level:
        parts.append(f'messageType == {check.level.lower()}')
    return '(' + ' AND '.join(parts) + ')'


def predicate(checks: Iterable[Check]) -> str:
    """Return every check's clause; with any debug check, keep the others to default, error and fault lines."""
    checks = list(checks)
    clauses = list(dict.fromkeys(clause(c) for c in checks if not c.debug))
    debug = list(dict.fromkeys(clause(c) for c in checks if c.debug))
    frames = f'eventMessage BEGINSWITH {quote(FRAME_PREFIX)}'
    if not debug:
        return ' OR '.join([*clauses, frames])
    plain = '(' + ' OR '.join([*clauses, frames]) + ') AND messageType != info AND messageType != debug'
    return ' OR '.join([f'({plain})', *debug])


def log_show(archive: str, query: str, start: str | None, end: str | None, debug: bool = False) -> Iterator[dict]:
    """Yield the archive's matching records, as `log show --style ndjson` prints them."""
    cmd = ['/usr/bin/log', 'show', '--archive', archive, '--style', 'ndjson', '--predicate', query]
    if debug:
        cmd += ['--info', '--debug']
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
    for record in joined(records):
        event = Event.from_ndjson(record)
        first, last = min(first or event.time, event.time), max(last or event.time, event.time)
        if event.message.startswith(FRAME_PREFIX):
            frame_lines.append(f'{record["timestamp"]} {event.message}')
        for check in checks:
            fields = match(check, event, regexes[check.name])
            if fields is not None:
                by_name[check.name].append(fields)
    minutes = (last - first).total_seconds() / 60 if first else 0.0
    return [evaluate(c, by_name[c.name], minutes, by_name, last) for c in checks], frame_lines


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


def all_checks(label: str | None) -> list[Check]:
    """Return the selected groups' checks."""
    return [c for group in selected(label).values() for c in group]


def checklist(label: str | None) -> str:
    steps = usb.STEPS if label == usb.LABEL else CHECKLIST[''] + (CHECKLIST.get(label, []) if label else [])
    return '\n'.join([f'Boot {label or "(no label)"}: manual steps before pulling the logs', *steps, ''])


def split_boots(records: list[dict]) -> dict[str, list[dict]]:
    """Return the records by boot UUID, the boots in the order their first lines come."""
    by_boot: dict[str, list[dict]] = {}
    for record in records:
        by_boot.setdefault(record.get('bootUUID') or '', []).append(record)
    return by_boot


def starts(records: list[dict]) -> bool:
    return any((r.get('eventMessage') or '').startswith(KERNEL_LINE) for r in records)


def usb_boot(by_boot: dict[str, list[dict]], wanted: str | None) -> str:
    """Return the boot the usb group judges: the one `--boot` names, else the last that starts in the archive.

    A boot starts in the archive when its kernel version line (uptime 0) is there; with none that does, the last boot.
    """
    if wanted:
        found = [boot for boot in by_boot if boot.upper().startswith(wanted.upper())]
        if len(found) != 1:
            listed = ', '.join(boot[:8] for boot in by_boot)
            raise SystemExit(f"--boot {wanted}: {len(found)} of the archive's boots match ({listed})")
        return found[0]
    whole = [boot for boot, records in by_boot.items() if starts(records)]
    return (whole or list(by_boot))[-1]


def boot_text(boot: str, records: list[dict]) -> str:
    times = [Event.from_ndjson(r).time for r in records]
    span = f'{boot[:8]} {min(times):%Y-%m-%d %H:%M:%S} to {max(times):%H:%M:%S}'
    return span + ('' if starts(records) else ', no kernel line (it started before the archive)')


def boot_row(by_boot: dict[str, list[dict]], boot: str) -> Result:
    others = [other for other in by_boot if other != boot]
    observed = f'boot {boot_text(boot, by_boot[boot])}; {len(others)} other boot{"" if len(others) == 1 else "s"} in '
    observed += 'the archive' + (
        ': ' + '; '.join(boot_text(other, by_boot[other]) for other in others) if others else ''
    )
    return Result(Check('usb.boot', usb.OWNER, '', REPORT), INFO, observed)


def evaluate_all(
    records: Iterable[dict], crashes: str | None, label: str | None = None, boot: str | None = None
) -> list[tuple[str, list[Result]]]:
    """Return every group's rows; the usb group judges one boot (`boot`, a UUID prefix, or the last that starts)."""
    groups_in = selected(label)
    records = list(records)
    usb_checks = groups_in['usb']
    names = {c.name for c in usb_checks}
    results, frame_lines = run_checks(records, [c for c in all_checks(label) if c.name not in names])
    by_boot = split_boots(records)
    judged = usb_boot(by_boot, boot) if by_boot else ''
    usb_records = by_boot.get(judged, [])
    usb_results, _ = run_checks(usb_records, usb_checks)
    by_check = {r.check.name: r for r in results + usb_results}
    end = max((Event.from_ndjson(r).time for r in usb_records), default=None)
    groups = [(name, rolled_up([by_check[c.name] for c in group])) for name, group in groups_in.items()]
    groups.append(('cpu-summary', cpu_rows(by_check)))
    groups.append(('commit-summary', commit_rows(by_check)))
    groups.append(('launch-summary', launch_rows(by_check)))
    groups.append(('springboard-life', springboard_rows(by_check)))
    groups.append(('pack-image-boot', pack_image_rows(by_check)))
    groups.append(('memory-watch', memory_rows(by_check)))
    groups.append(('shared-cache-nested', cache_rows(by_check)))
    head = [boot_row(by_boot, judged)] if by_boot else []
    groups.append(('usb-layers', [*head, *usb.rows(by_check, crashes, end)]))
    groups.append(('frames', frame_rows(frame_lines)))
    groups.append(('crashes', crash_rows(crashes)))
    groups.append(('owed-summary', owed_summary(by_check, [boot_text(b, r) for b, r in by_boot.items()])))
    return groups


def main(argv: list[str] | None = None) -> int:
    """Print the table; exit 1 when any check fails."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('archive', help='a pulled .logarchive (absolute path under bazel run)')
    parser.add_argument('--crashes', help='a directory of pulled crash reports (.ips)')
    parser.add_argument('--tsv', help='also write the table, with every check source, as TSV here')
    parser.add_argument('--start', help="log show's --start, e.g. '2026-11-07 09:00:00'")
    parser.add_argument('--end', help="log show's --end")
    parser.add_argument(
        '--boot-label',
        choices=['A', 'B', 'K', usb.LABEL],
        help='the morning boot: A, B tiled cache, K kprof; U the USB boot (its own manual steps)',
    )
    parser.add_argument('--boot', help="the usb group's boot, a boot UUID prefix (default: the last that starts)")
    parser.add_argument('--ndjson', action='store_true', help='the input is `log show --style ndjson` output')
    parser.add_argument('--predicate', action='store_true', help='print the log show predicate and exit')
    args = parser.parse_args(argv)
    checks = all_checks(args.boot_label)
    if args.predicate:
        print(predicate(checks))
        return 0
    if args.ndjson:
        records = read_ndjson(args.archive)
    else:
        records = log_show(args.archive, predicate(checks), args.start, args.end, any(c.debug for c in checks))
    print(checklist(args.boot_label))
    groups = evaluate_all(records, args.crashes, args.boot_label, args.boot)
    print(headline(groups))
    print(blocking(groups))
    judged = [r for group, results in groups if group == 'usb-layers' for r in results if r.check.name == 'usb.boot']
    print(*(f'The usb group judges {r.observed}\n' for r in judged), sep='')
    print(table(groups))
    if args.tsv:
        write_tsv(args.tsv, groups)
    verdicts = [r.verdict for _, results in groups for r in results]
    print(' '.join(f'{v} {verdicts.count(v)}' for v in sorted(set(verdicts))))
    return 1 if FAIL in verdicts else 0


if __name__ == '__main__':
    sys.exit(main())
