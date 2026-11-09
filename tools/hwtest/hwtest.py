#!/usr/bin/env python3
"""HW Test's runs against the runs before them (README.md).

    bazel run //tools/hwtest -- [--history DIR] [REPORT.json ...]
    bazel run //tools/hwtest -- --import-log hwtest-bootN.txt --boot-log bootN.txt --build 24B119 [--nak-debug V]

With no report named, the newest run of each app and iOS build in the history. pull_logs fetches the card's reports
into the history and prints this for the runs it found new. --import-log adds a run from before the app wrote
reports, from its HWTEST lines (fromlog.py).
"""

import argparse
import json
import os
import sys
from collections.abc import Callable
from pathlib import Path

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(os.path.abspath(__file__)).parents[2]))
from tools.common import paths
from tools.hwtest import compare, fetch, fromlog, posters
from tools.hwtest.reports import Report, history

SUMMARY = 'hwtest-summary.txt'


def history_dir() -> Path:
    return paths.state_dir() / 'device-logs' / 'hwtest'


def summaries(reports: list[Report], chosen: list[Report]) -> str:
    """Each chosen report against the earlier runs of its app on its build."""
    texts = []
    for latest in chosen:
        earlier = [r for r in reports if r.key == latest.key and r.started < latest.started]
        texts.append(compare.summary(latest, earlier))
    return '\n\n'.join(texts)


def newest_per_key(reports: list[Report]) -> list[Report]:
    newest: dict[tuple[str, str], Report] = {}
    for report in reports:
        newest[report.key] = report
    return list(newest.values())


def after_pull(data: Path, switch_folder: Path, out: Path, log: Callable[..., None]) -> None:
    """Fetch the card's reports into the history and `out`, and log the summary of the newest new run of each app.

    The lock-screen poster's newest landscape render is read on every pull, HW Test run or not (posters.py).
    """
    poster = posters.summary_line(posters.observe(data, out))
    if poster:
        log('hwtest:', poster)
    archive = out / 'boot.logarchive'
    changed = {path.name for path in fetch.fetch(data, switch_folder, history_dir(), out, archive)}
    if not changed:
        log('hwtest: no new HW Test run on the card')
        return
    reports = history(history_dir())
    text = summaries(reports, newest_per_key([r for r in reports if r.path.name in changed]))
    (out / SUMMARY).write_text(text + '\n')
    log(f'hwtest: {len(changed)} new or updated run reports -> {history_dir()}\n{text}')


def import_log(args: argparse.Namespace, folder: Path) -> Path:
    """Write the run in `args.import_log`'s HWTEST lines into the history as <boot>-imported.json."""
    boot, started = fromlog.boot_of(args.boot_log)
    identity = {'bundle_id': args.bundle, 'build': args.build, 'ios': args.build_ios, 'nak_debug': args.nak_debug}
    report = fromlog.report(Path(args.import_log).read_text().splitlines(), identity, boot, started)
    folder.mkdir(parents=True, exist_ok=True)
    target = folder / (report['run']['id'] + '.json')
    target.write_text(json.dumps(report, indent=1, sort_keys=True) + '\n')
    return target


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('reports', nargs='*', type=Path)
    parser.add_argument('--history', type=Path, default=None)
    parser.add_argument('--import-log', type=Path)
    parser.add_argument('--boot-log', type=Path)
    parser.add_argument('--build', default='')
    parser.add_argument('--build-ios', default='')
    parser.add_argument('--bundle', default=fetch.BUNDLES[0])
    parser.add_argument('--nak-debug', default='')
    args = parser.parse_args(argv)
    if args.import_log:
        if not (args.boot_log and args.build):
            parser.error('--import-log needs --boot-log and --build')
        print('imported ->', import_log(args, args.history or history_dir()))
        return 0
    reports = history(args.history or history_dir())
    held = {r.path.resolve() for r in reports}
    reports = sorted(
        reports + [Report.load(path) for path in args.reports if path.resolve() not in held], key=lambda r: r.started
    )
    named = {path.resolve() for path in args.reports}
    chosen = [r for r in reports if r.path.resolve() in named] if named else newest_per_key(reports)
    if not chosen:
        print('no HW Test reports in', args.history or history_dir())
        return 1
    print(summaries(reports, chosen))
    return 0


if __name__ == '__main__':
    sys.exit(main())
