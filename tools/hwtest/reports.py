"""HW Test's run reports (hw_test_app/core/Report.swift, schema 2) and the sidecar the Mac writes beside each one.

The device writes <boot uuid>-<stamp>.json before and after every test; fetch.py copies it into the history folder and
adds <boot uuid>-<stamp>.mac.json: the commit deployed to the card, and the crashes and panics of that boot.
"""

import json
from dataclasses import dataclass, field
from pathlib import Path

SIDECAR = '.mac.json'


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
        return cls(path, json.loads(path.read_text()), mac)

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
