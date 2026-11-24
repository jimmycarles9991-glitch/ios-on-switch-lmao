"""A session's records: the log lines and signposts the report reads, from a pulled .logarchive or ndjson files.

One `log show` pulls the log lines and one `log show --signpost` the signposts, each through a predicate naming only
what a section reads, so a 270 MB archive reads in seconds.
"""

import json
import subprocess
from collections.abc import Iterable, Iterator
from dataclasses import dataclass
from datetime import datetime, timedelta

TICK_HZ = 19.2e6  # mach absolute time on the Switch: the Tegra X1 generic timer

KERNEL_PREFIXES = (
    'SwitchFrame',
    'SwitchDisplay stats',
    'SwitchSched',
    'SwitchPower',
    'SwitchKprof',
    'SwitchStall',
    'SwitchUnnest',
    'SwitchMark',
    'SwitchRecorder',
    'tegra-gm20b: stats ',
    'tegra-gm20b: exec by process',
    'tegra-gm20b: jobs ',
    'tegra-gm20b: slow import',
    'tegra-gm20b: compositor frame missed',
)
METAL_PARTS = ('SwitchFrameUser', 'long span', 'the pack lacks', 'made by the shader compiler', ' gm20b 5s ')
FRONTBOARD_PARTS = (
    'Asked to bootstrap a new process with identity',
    'Bootstrap success',
    'content state changed: ready',
)


def _any(field: str, verb: str, parts: Iterable[str]) -> str:
    return '(' + ' OR '.join(f'{field} {verb} "{p}"' for p in parts) + ')'


LOG_PREDICATE = ' OR '.join(
    [
        '(process BEGINSWITH "kernel" AND ' + _any('eventMessage', 'BEGINSWITH', KERNEL_PREFIXES) + ')',
        '(subsystem == "com.brunostjohn.switch.TegraGM20BMetal" AND '
        + _any('eventMessage', 'CONTAINS', METAL_PARTS)
        + ')',
        '(process == "SwitchShaderCompiler" AND eventMessage BEGINSWITH "SwitchShaderCompiler: made in")',
        '(subsystem == "com.brunostjohn.switch.hwtest" AND '
        + _any('eventMessage', 'BEGINSWITH', ('HWTEST ', 'SwitchMark: '))
        + ')',
        (
            '(subsystem == "com.apple.hangtracer" AND eventMessage BEGINSWITH "Hang detected" AND eventMessage '
            'CONTAINS "always-on")'
        ),
        '(subsystem == "com.apple.FrontBoard" AND ' + _any('eventMessage', 'CONTAINS', FRONTBOARD_PARTS) + ')',
        (
            '(subsystem == "com.apple.tailspin" OR eventMessage CONTAINS "TailspinStatus" OR eventMessage '
            'CONTAINS "microstackshot")'
        ),
    ]
)
SIGNPOST_PREDICATE = (
    'eventType == "signpostEvent" AND ((subsystem == "com.apple.hangtracer" AND category == "signpost_hang") OR '
    '(subsystem == "com.apple.coreanimation" AND category IN {"IOMFBServer.Stalls", "Transaction", '
    '"Transaction.Stalls", "Metal.Pipelines"}) OR (subsystem == "com.apple.UIKit" AND category == '
    '"UpdateCycle.Stalls"))'
)


@dataclass(frozen=True)
class Rec:
    """One log line or signpost."""

    time: datetime
    mach: int
    boot: str
    proc: str
    pid: int
    tid: int
    subsystem: str
    category: str
    name: str  # the signpost's name; '' for a log line
    kind: str  # the signpost's type (begin, end, event); '' for a log line
    ident: int  # the signpost's id
    msg: str


def from_ndjson(record: dict) -> Rec | None:
    """Return the record of one `log show --style ndjson` object, or None for anything but a line or a signpost."""
    if record.get('eventType') not in {'logEvent', 'signpostEvent'}:
        return None
    proc = record.get('processImagePath', '').rsplit('/', 1)[-1]
    return Rec(
        time=datetime.fromisoformat(record['timestamp']),
        mach=int(record.get('machTimestamp', 0)),
        boot=record.get('bootUUID', ''),
        proc='kernel' if proc.startswith('kernel') else proc,
        pid=int(record.get('processID', 0)),
        tid=int(record.get('threadID', 0)),
        subsystem=record.get('subsystem', ''),
        category=record.get('category', ''),
        name=record.get('signpostName', ''),
        kind=record.get('signpostType', ''),
        ident=int(record.get('signpostID', 0)),
        msg=record.get('eventMessage', ''),
    )


def parse_lines(lines: Iterable[str], images: dict[str, str]) -> Iterator[Rec]:
    """Yield the records of ndjson lines, noting in images the path of every image UUID a record names."""
    for line in lines:
        if not line.startswith('{'):
            continue
        record = json.loads(line)
        for kind in ('process', 'sender'):
            if uuid := record.get(f'{kind}ImageUUID'):
                images.setdefault(uuid.upper(), record.get(f'{kind}ImagePath', ''))
        if rec := from_ndjson(record):
            yield rec


def log_show(archive: str, signposts: bool, images: dict[str, str], window: tuple[str | None, str | None]) -> list[Rec]:
    cmd = ['/usr/bin/log', 'show', '--archive', archive, '--style', 'ndjson']
    cmd += ['--signpost', '--predicate', SIGNPOST_PREDICATE] if signposts else ['--predicate', LOG_PREDICATE]
    for flag, value in zip(('--start', '--end'), window, strict=True):
        if value:
            cmd += [flag, value]
    with subprocess.Popen(cmd, stdout=subprocess.PIPE, text=True, errors='replace') as proc:
        out = list(parse_lines(proc.stdout, images))
    if proc.returncode:
        raise SystemExit(f'log show failed ({proc.returncode}): {" ".join(cmd[:4])}')
    return out


def read(paths: list[str], start: str | None = None, end: str | None = None) -> tuple[list[Rec], dict[str, str]]:
    """Return every record of .logarchives (both queries) or ndjson files in time order, and the images they name."""
    out: list[Rec] = []
    images: dict[str, str] = {}
    for path in paths:
        if path.rstrip('/').endswith('.logarchive'):
            out += log_show(path, False, images, (start, end))
            out += log_show(path, True, images, (start, end))
        else:
            with open(path, errors='replace') as f:
                out += parse_lines(f, images)
    out.sort(key=lambda r: r.time)
    return out, images


def by_boot(recs: list[Rec]) -> dict[str, list[Rec]]:
    """Split the records by boot session, in the order the boots appear."""
    boots: dict[str, list[Rec]] = {}
    for r in recs:
        boots.setdefault(r.boot, []).append(r)
    return boots


def wall(r: Rec, mach: int) -> datetime:
    """Return the wall time of a mach time near the record, from the record's own pair of the two.

    Mach absolute time stops while the device sleeps, so one anchor for a whole boot would drift by every sleep.
    """
    return r.time + timedelta(seconds=(mach - r.mach) / TICK_HZ)


def ms(ticks: int) -> float:
    return ticks * 1000 / TICK_HZ
