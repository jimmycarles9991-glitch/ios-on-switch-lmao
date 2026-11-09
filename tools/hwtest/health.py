"""What a boot did besides HW Test's run, from the card's Data volume: crash reports, saved panics, pairing records."""

import json
import plistlib
import re
from collections import Counter
from datetime import UTC, datetime
from pathlib import Path

CRASH, JETSAM = '309', '298'
# A Core Animation client that aborted in +[CADisplay displays]: the display list it expected was not there.
CADISPLAY = '+[CADisplay '
PANIC = re.compile(r'^panic-(\d{4}-\d{2}-\d{2}-\d{6})(?:-damaged)?\.log$')
# lockdownd's pair records, one per trusted host (Data is /private/var).
PAIR_RECORDS = ('root/Library/Lockdown/pair_records', 'db/lockdown/pair_records')
# tools/deploy/system_apps.py's INSTALLED and APP_STATES: installd's bundles, and the system apps it still wants (1).
INSTALLED = 'containers/Bundle/Application'
APP_STATES = 'installd/Library/MobileInstallation/BackedUpState/SystemAppInstallState.plist'


def report_body(text: str) -> tuple[dict, dict]:
    """Return an .ips report's header line and its JSON body ({} when the body is not JSON)."""
    header, _, body = text.partition('\n')
    try:
        return json.loads(header), json.loads(body)
    except ValueError:
        return {}, {}


def crashes(directory: Path, boot: str) -> dict:
    """Count the boot's crash reports: crashes by process, jetsam events, the rest by bug type; list HW Test's."""
    crashed: Counter = Counter()
    cadisplay: Counter = Counter()
    other: Counter = Counter()
    jetsams = 0
    hwtest = []
    for path in sorted(directory.glob('*.ips')) if directory.is_dir() else []:
        text = path.read_text(errors='replace')
        header, body = report_body(text)
        if body.get('bootSessionUUID', '').upper() != boot.upper():
            continue
        kind = str(header.get('bug_type') or body.get('bug_type') or '')
        process = body.get('procName') or header.get('name') or path.stem.split('-')[0]
        if kind == CRASH:
            crashed[process] += 1
            if CADISPLAY in text:
                cadisplay[process] += 1
            if process.startswith('SwitchHWTest'):
                exception = body.get('exception') or {}
                hwtest.append('{}: {} {}'.format(path.name, exception.get('type', ''), exception.get('signal', '')))
        elif kind == JETSAM:
            jetsams += 1
        else:
            other[kind] += 1
    return {
        'crashes': dict(crashed),
        'cadisplay_aborts': dict(cadisplay),
        'jetsams': jetsams,
        'other': dict(other),
        'hwtest_crashes': hwtest,
    }


def panics(folder: Path, since: datetime) -> list[str]:
    """List the panics the kernel saved (SwitchPanicSave.c, panic-<UTC>.log) at or after `since`."""
    found = []
    for path in sorted(folder.glob('panic-*.log')) if folder.is_dir() else []:
        match = PANIC.match(path.name)
        if match and datetime.strptime(match[1], '%Y-%m-%d-%H%M%S').replace(tzinfo=UTC) >= since:
            found.append(path.name)
    return found


def pairing(data: Path) -> int:
    """How many hosts lockdownd trusts."""
    return sum(len(list((data / folder).glob('*.plist'))) for folder in PAIR_RECORDS if (data / folder).is_dir())


def plist(path: Path) -> dict:
    try:
        return plistlib.loads(path.read_bytes())
    except (OSError, plistlib.InvalidFileException, ValueError):
        return {}


def apps(data: Path) -> dict:
    """Count installd's app bundles, the placeholders among them (no executable) and the system apps it still wants."""
    installed = placeholders = 0
    for app in sorted((data / INSTALLED).glob('*/*.app')) if (data / INSTALLED).is_dir() else []:
        executable = plist(app / 'Info.plist').get('CFBundleExecutable')
        if executable and (app / executable).is_file():
            installed += 1
        else:
            placeholders += 1
    wanted = sum(1 for state in plist(data / APP_STATES).values() if state == 1)
    return {'installed': installed, 'placeholders': placeholders, 'system_apps_wanted': wanted}


def parse_time(text: str) -> datetime | None:
    try:
        return datetime.fromisoformat(text)
    except ValueError:
        return None
