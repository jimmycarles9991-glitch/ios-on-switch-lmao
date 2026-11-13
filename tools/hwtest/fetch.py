"""Copy HW Test's run reports off the card's Data volume into the Mac's history, each with its sidecar.

The app keeps its reports in its data container (Documents/hwtest), which needs no change to the OS: the container is
found by its metadata plist. The sidecar holds what the device cannot see: the commit deploy.py recorded on the card,
the boot's crash reports and saved panics, and how many hosts the device is paired with.
"""

import json
import plistlib
import shutil
from datetime import UTC, datetime, timedelta
from pathlib import Path

from tools.hwtest import bootlog, health, posters
from tools.hwtest.reports import Report, is_report, sidecar_of

BUNDLES = ('com.brunostjohn.switch.hwtest', 'com.brunostjohn.switch.hwtest27e')
CONTAINERS = 'mobile/Containers/Data/Application'
METADATA = '.com.apple.mobile_container_manager.metadata.plist'
CRASH_REPORTS = 'mobile/Library/Logs/CrashReporter'
DEPLOYED = 'deployed.json'


def containers(data: Path) -> dict[str, Path]:
    """HW Test's data containers on the Data volume by bundle ID (the 27 debug image has an arm64e twin)."""
    found = {}
    for metadata in sorted((data / CONTAINERS).glob('*/' + METADATA)) if (data / CONTAINERS).is_dir() else []:
        try:
            identifier = plistlib.loads(metadata.read_bytes()).get('MCMMetadataIdentifier')
        except (OSError, plistlib.InvalidFileException, ValueError):
            continue
        if identifier in BUNDLES:
            found[identifier] = metadata.parent
    return found


def copy_gpu_files(container: Path, out: Path, identifier: str) -> None:
    """Copy the GPU scenes' failed readbacks (got, want and diff PNGs, the CA sweep's states.txt) and the UI tests'
    failed pictures (Documents/ui) to out."""
    for folder, kind in (('gpu', 'hwtest-gpu'), ('ui', 'hwtest-ui')):
        files = container / 'Documents' / folder
        if files.is_dir():
            shutil.copytree(files, out / kind / identifier, copy_function=shutil.copyfile, dirs_exist_ok=True)


def sidecar(report: Report, data: Path, switch_folder: Path, archive: Path | None = None) -> dict:
    """Return what the Mac adds to a report: the deployed commit, the boot's crashes, panics and boot time."""
    deployed_path = switch_folder / DEPLOYED
    deployed = json.loads(deployed_path.read_text()) if deployed_path.is_file() else None
    boot_time = health.parse_time(report.identity.get('boot_time', ''))
    if deployed and boot_time and (health.parse_time(deployed.get('deployed_at', '')) or boot_time) > boot_time:
        deployed = {**deployed, 'commit': '', 'note': 'deployed after this boot: the commit that ran is unknown'}
    started = health.parse_time(report.started) or datetime.now(UTC)
    boot = report.run.get('boot_uuid', '')
    return {
        'deployed': deployed,
        'health': {
            **health.crashes(data / CRASH_REPORTS, boot),
            'boot_s': bootlog.boot_seconds(archive, boot, boot_time) if archive else None,
            'panics': [] if report.finished else health.panics(switch_folder, started - timedelta(minutes=1)),
            'pair_records': health.pairing(data),
            'apps': health.apps(data),
            'lock_poster': posters.observe(data),
        },
        'pulled': datetime.now(UTC).isoformat(timespec='seconds'),
    }


def fetch(
    data: Path, switch_folder: Path, history: Path, out: Path | None = None, archive: Path | None = None
) -> list[Path]:
    """Copy every report the card holds into `history` (and `out`, with the GPU scenes' files); return the new ones.

    A report the history already holds byte for byte keeps its sidecar: a later pull may no longer find that boot's
    crash reports on the card.
    """
    history.mkdir(parents=True, exist_ok=True)
    changed = []
    for identifier, container in containers(data).items():
        if out:
            copy_gpu_files(container, out, identifier)
        for source in sorted((container / 'Documents/hwtest').glob('*.json')):
            if not is_report(source):
                continue
            target = history / source.name
            if out:
                out.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, out / source.name)
            if target.is_file() and target.read_bytes() == source.read_bytes():
                continue
            shutil.copyfile(source, target)
            try:
                report = Report.load(target)
            except ValueError:
                continue
            mac = sidecar(report, data, switch_folder, archive)
            sidecar_of(target).write_text(json.dumps(mac, indent=1) + '\n')
            changed.append(target)
    return changed
