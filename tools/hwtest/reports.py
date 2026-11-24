"""HW Test's run reports (apps/hwtest/core/Report.swift, schema 2) and the sidecar the Mac writes beside each one.

The device keeps a running run as <boot uuid>-<stamp>.journal (apps/hwtest/core/RunJournal.swift) and writes an ended
one as <boot uuid>-<stamp>.json; fetch.py copies each into the history folder as its json and adds
<boot uuid>-<stamp>.mac.json: the commit deployed to the card, and the crashes and panics of that boot.
"""

import json
from dataclasses import dataclass, field
from pathlib import Path

from tools.devicerun import renamed

SIDECAR = '.mac.json'
JOURNAL = '.journal'


@dataclass
class Report:
    """One run as the device wrote it, with what the Mac knows about it."""

    path: Path
    data: dict
    mac: dict = field(default_factory=dict)

    @classmethod
    def load(cls, path: Path) -> 'Report':
        sidecar = sidecar_of(path)
        mac = json.loads(sidecar.read_text()) if sidecar.is_file() else {}
        return cls(path, renamed.current_report(json.loads(path.read_text())), mac)

    @property
    def run(self) -> dict:
        return self.data.get('run', {})

    @property
    def identity(self) -> dict:
        return self.data.get('identity', {})

    @property
    def key(self) -> tuple[str, str]:
        """Reports compare only within one app and one iOS build: 17 and 27, arm64 and arm64e never mix."""
        return self.identity.get('bundle_id', ''), self.identity.get('build', '')

    @property
    def finished(self) -> bool:
        return self.run.get('status') == 'done'

    @property
    def current(self) -> str | None:
        return self.run.get('current')

    @property
    def started(self) -> str:
        return self.run.get('started', '')

    @property
    def results(self) -> dict[str, dict]:
        return {entry['id']: entry for entry in self.data.get('results', [])}

    @property
    def commit(self) -> str:
        deployed = self.mac.get('deployed') or {}
        commit = deployed.get('commit', '')[:12]
        if not commit:
            return 'commit unknown'
        return commit + ('+dirty' if deployed.get('dirty') else '')

    def label(self) -> str:
        return '{} on {} ({})'.format(self.started[:19].replace('T', ' '), self.identity.get('build', '?'), self.commit)


def sidecar_of(path: Path) -> Path:
    return path.with_name(path.stem + SIDECAR)


def is_report(path: Path) -> bool:
    return path.suffix == '.json' and not path.name.endswith(SIDECAR) and not path.name.startswith('.')


def history(directory: Path) -> list[Report]:
    """Every readable schema-2 report in `directory`, oldest run first."""
    reports = []
    for path in sorted(directory.glob('*.json')) if directory.is_dir() else []:
        if not is_report(path):
            continue
        try:
            report = Report.load(path)
        except (OSError, ValueError):
            continue
        if report.data.get('schema') == 2:
            reports.append(report)
    return sorted(reports, key=lambda report: report.started)


def from_journal(data: bytes) -> dict | None:
    """Return the report a run's journal holds, as the app reads it (RunJournal.report(from:)).

    The first line is the report and each whole line after it one change. Reading stops at a line without its newline
    (the app died writing it) or one that is not a change; None when the first line is not a report.
    """
    lines = data.split(b'\n')[:-1]
    try:
        report = json.loads(lines[0]) if lines else None
    except ValueError:
        return None
    if not isinstance(report, dict) or not isinstance(report.get('run'), dict):
        return None
    for line in lines[1:]:
        try:
            change = json.loads(line)
        except ValueError:
            break
        if not isinstance(change, dict) or not apply(report, change):
            break
    return report


def apply(report: dict, change: dict) -> bool:
    """Apply one journal line to `report` as Report.apply does; False for a line that is no change."""
    run, results = report['run'], report.setdefault('results', [])
    if 'begin' in change:
        run['current'] = change['begin']
    elif 'result' in change:
        entry = change['result']
        index = next((i for i, e in enumerate(results) if e.get('id') == entry.get('id')), len(results))
        results[index : index + 1] = [entry]
        if run.get('current') == entry.get('id'):
            run.pop('current', None)
        verdicts = [e.get('verdict') for e in results]
        report['summary'] = {
            'pass': verdicts.count('PASS'),
            'fail': verdicts.count('FAIL'),
            'skip': verdicts.count('SKIP'),
            'crashed': sum(e.get('metrics', {}).get('crashed') is True for e in results),
            'planned': len(run.get('order', [])),
        }
    elif 'interruption' in change:
        if change['interruption']:
            run['interruption'] = change['interruption']
        else:
            run.pop('interruption', None)
    elif 'finished' in change:
        run.update(status='done', finished=change['finished'])
        run.pop('current', None)
        run.pop('interruption', None)
    else:
        return False
    return True
