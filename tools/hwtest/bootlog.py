"""A boot's time to SpringBoard from the pulled logarchive: uptime 0 to backboardd's "Dismissing boot logo".

The app cannot read other processes' logs from its sandbox, so the Mac reads it from the archive pull_logs built. The
start is the report's kern.boottime; `log show` is scoped to that boot's first hour so it skips earlier boots.
"""

import json
import subprocess
from datetime import datetime, timedelta
from pathlib import Path

PREDICATE = 'process == "backboardd" AND eventMessage CONTAINS "Dismissing boot logo"'
LOG = '/usr/bin/log'


def command(archive: Path, boot_time: datetime) -> list[str]:
    """Return `log show` over the hour after `boot_time` (log show takes local time)."""
    local = boot_time.astimezone()
    return [
        LOG,
        'show',
        '--archive',
        str(archive),
        '--start',
        local.strftime('%Y-%m-%d %H:%M:%S'),
        '--end',
        (local + timedelta(hours=1)).strftime('%Y-%m-%d %H:%M:%S'),
        '--predicate',
        PREDICATE,
        '--style',
        'ndjson',
    ]


def dismissed(lines: list[str], boot_uuid: str) -> datetime | None:
    """Return the first boot-logo dismissal of `boot_uuid` among `log show` ndjson lines."""
    for line in lines:
        try:
            event = json.loads(line)
        except ValueError:
            continue
        if event.get('bootUUID', '').upper() == boot_uuid.upper() and 'timestamp' in event:
            return datetime.strptime(event['timestamp'], '%Y-%m-%d %H:%M:%S.%f%z')
    return None


def boot_seconds(archive: Path, boot_uuid: str, boot_time: datetime | None) -> float | None:
    """Return the seconds from `boot_time` to the boot logo's dismissal, or None when the archive lacks it."""
    if not boot_time or not Path(archive).is_dir() or not Path(LOG).is_file():
        return None
    result = subprocess.run(command(archive, boot_time), capture_output=True, text=True, check=False, timeout=300)
    when = dismissed(result.stdout.splitlines(), boot_uuid)
    return round((when - boot_time).total_seconds(), 1) if when else None
