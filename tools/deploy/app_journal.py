"""The overlay apps a deploy added, changed or removed, registered one by one at the next boot instead of a migration.

The deploy compares what LaunchServices registers for each app in the overlay's /Applications (registration.py) before
and after the sync, and records in JOURNAL each app to register and each to unregister, every entry with a token of its
own. At boot switch-appregd (drivers/SwitchAppRegistration) applies them the way installd re-registers a built-in app,
and lists in APPLIED the token of each entry LaunchServices has saved. The deploy writes Data with owners off, so
switch-appregd, which runs as mobile, cannot rewrite the journal; it keeps APPLIED in mobile's Library, and the next
deploy drops the entries listed there.

On iOS 27 launchd starts switch-appregd at every boot. On iOS 17 only the BlueTool stand-in can start it, and
switch-no-bt hides the stand-in, so with it there is nothing to apply the journal (NextBoot.registers_apps).
"""

import plistlib
import uuid
from pathlib import Path
from typing import NamedTuple

from tools.common import files
from tools.deploy.registration import App
from tools.install import host

JOURNAL = 'Switch/app-registration.plist'
APPLIED = 'mobile/Library/com.brunostjohn.switch.app-registration.applied.plist'
APPLICATIONS = '/Applications'
REGISTER, UNREGISTER = 'Register', 'Unregister'
IDENTIFIER, TOKEN = 'BundleIdentifier', 'Token'
LAUNCHD_FROM = 27
NO_BLUETOOTH = 'switch-no-bt'


def _load(path: Path) -> object:
    try:
        return plistlib.loads(path.read_bytes())
    except (OSError, plistlib.InvalidFileException, ValueError):
        return None


def _path(name: str) -> str:
    return f'{APPLICATIONS}/{name}'


def changes(before: dict[str, App], after: dict[str, App]) -> tuple[dict[str, str], dict[str, str]]:
    """Return the apps to register and to unregister, each {path on the device: bundle identifier}."""
    kept = {app.identifier for app in after.values()}
    register = {_path(name): app.identifier for name, app in after.items() if before.get(name) != app}
    unregister = {_path(name): app.identifier for name, app in before.items() if app.identifier not in kept}
    return register, unregister


def pending(data_root: Path) -> dict[str, dict[str, dict]]:
    """Return the journal's entries switch-appregd has not applied yet, {stage: {path: entry}}."""
    journal = _load(Path(data_root) / JOURNAL)
    applied = _load(Path(data_root) / APPLIED)
    done = set(applied) if isinstance(applied, list) else set()
    journal = journal if isinstance(journal, dict) else {}
    return {
        stage: {
            path: entry
            for path, entry in (journal.get(stage) or {}).items()
            if isinstance(entry, dict) and entry.get(TOKEN) not in done
        }
        for stage in (UNREGISTER, REGISTER)
    }


def _entry(identifier: str) -> dict:
    return {IDENTIFIER: identifier, TOKEN: str(uuid.uuid4()).upper()}


def record(data_root: Path, before: dict[str, App], after: dict[str, App], log: host.Log) -> int:
    """Merge this deploy's changes into the journal's pending entries; return how many entries are pending."""
    register, unregister = changes(before, after)
    journal = pending(data_root)
    present = {_path(name) for name in after}
    kept = {app.identifier for app in after.values()}
    journal[REGISTER] = {path: entry for path, entry in journal[REGISTER].items() if path in present}
    journal[UNREGISTER] = {
        path: entry for path, entry in journal[UNREGISTER].items() if entry.get(IDENTIFIER) not in kept
    }
    journal[REGISTER] |= {path: _entry(identifier) for path, identifier in register.items()}
    journal[UNREGISTER] |= {path: _entry(identifier) for path, identifier in unregister.items()}
    count = len(journal[REGISTER]) + len(journal[UNREGISTER])
    path = Path(data_root) / JOURNAL
    if count:
        path.parent.mkdir(parents=True, exist_ok=True)
        files.write_bytes(path, plistlib.dumps(journal, sort_keys=True))
    else:
        path.unlink(missing_ok=True)
    if register or unregister:
        names = sorted(register.values()) + [f'-{identifier}' for identifier in sorted(unregister.values())]
        log(f'system apps: {", ".join(names)} registered at the next boot without a migration (<Data>/{JOURNAL})')
    elif count:
        log(f'system apps: {count} registration(s) from an earlier deploy still pending (<Data>/{JOURNAL})')
    return count


def clear(data_root: Path) -> None:
    """Drop the journal: a migration registers every app."""
    (Path(data_root) / JOURNAL).unlink(missing_ok=True)


def boot_arg(line: str, name: str) -> bool:
    """Whether `name` is on in a boot-args line, read as XNU's PE_parse_boot_argn reads a number: bare is 1."""
    for word in line.split():
        key, equals, value = word.partition('=')
        if key != name:
            continue
        if not equals:
            return True
        try:
            return int(value, 0) != 0
        except ValueError:
            return True
    return False


def ios_major(ios_version: str | None) -> int:
    """Return the major of an iOS version as the pins name it ("27.2"), 0 when there is none."""
    major = (ios_version or '').split('.')[0]
    return int(major) if major.isdigit() else 0


class NextBoot(NamedTuple):
    """The iOS version the card runs (the pins' "27.2") and its own boot-args (bootargs.txt)."""

    ios_version: str | None
    boot_args: str = ''

    def registers_apps(self) -> bool:
        """Whether switch-appregd runs: always from iOS 27, on iOS 17 only with Bluetooth's stand-in."""
        return ios_major(self.ios_version) >= LAUNCHD_FROM or not boot_arg(self.boot_args, NO_BLUETOOTH)
