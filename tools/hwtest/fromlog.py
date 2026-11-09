"""A run report (schema 2) from a run's HWTEST log lines, for runs from before the app wrote reports.

Boot 14's hwtest-boot14.txt becomes the history's first baseline this way. The lines don't carry subsystems, so a
test's ID prefix stands in for its subsystem, and a run without the suite's summary line is unfinished.
"""

import re
from datetime import UTC, datetime
from pathlib import Path

LINE = re.compile(r'HWTEST (?P<id>[a-z0-9_.]+) (?P<verdict>PASS|FAIL|SKIP)(?P<fields>(?: \S+)*)$')
BOOT = re.compile(
    r'^(?P<when>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\.\d+ \S+ \[0:0\] === system boot: (?P<uuid>[0-9A-F-]+)'
)
SUBSYSTEMS = (('gpu.', 'Metal / GPU'), ('metal.', 'Metal / GPU'), ('ui.', 'Compositor'), ('pacing.', 'Compositor'))
NUMBER = re.compile(r'-?\d+(?:\.\d+)?')


def value(text: str) -> object:
    if NUMBER.fullmatch(text):
        return float(text) if '.' in text else int(text)
    return text


def entry(match: re.Match) -> dict:
    fields: dict[str, str] = {}
    rest = match['fields'].strip()
    why = ''
    if ' why=' in f' {rest}':
        rest, _, why = f' {rest}'.partition(' why=')
    for field in rest.split():
        key, _, text = field.partition('=')
        fields[key] = text
    test = match['id']
    return {
        'id': test,
        'subsystem': next((name for prefix, name in SUBSYSTEMS if test.startswith(prefix)), ''),
        'name': test,
        'verdict': match['verdict'],
        'metrics': {k: value(v) for k, v in fields.items() if k not in ('hw', 'ms')},
        'proof': fields.get('hw', ''),
        'detail': why.replace('_', ' '),
        'milliseconds': float(fields.get('ms', 0)),
        'interactive': False,
        'finished': '',
        'log': match.group(0),
    }


def boot_of(boot_log: Path) -> tuple[str, str]:
    """Return the boot's UUID and start in UTC from a `log show` text's '=== system boot' line (this Mac's time)."""
    with Path(boot_log).open(errors='replace') as lines:
        for line in lines:
            if match := BOOT.match(line):
                when = datetime.strptime(match['when'], '%Y-%m-%d %H:%M:%S').astimezone(UTC)
                return match['uuid'], when.isoformat(timespec='milliseconds').replace('+00:00', 'Z')
    return '', ''


def report(lines: list[str], identity: dict, boot: str, started: str) -> dict:
    results = {}
    finished = False
    for line in lines:
        match = LINE.search(line.rstrip())
        if not match:
            continue
        if match['id'] == 'suite':
            finished = True
        else:
            results[match['id']] = entry(match)
    summary = {
        verdict.lower(): sum(r['verdict'] == verdict for r in results.values()) for verdict in ('PASS', 'FAIL', 'SKIP')
    }
    return {
        'schema': 2,
        'run': {
            'id': f'{boot}-imported',
            'boot_uuid': boot,
            'ios': int(identity.get('ios', 0) or 0),
            'started': started,
            'status': 'done' if finished else 'running',
            'current': None,
            'order': list(results),
        },
        'identity': {**identity, 'boot_uuid': boot},
        'summary': {**summary, 'crashed': 0, 'planned': len(results)},
        'results': list(results.values()),
    }
