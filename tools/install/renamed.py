"""What a card set up under the project's former names still holds, found by what it is rather than by those names.

  * FAT: a hekate ini in bootloader/ini carrying this boot entry's own line (boot/BUILD.bazel ios_ini), or whose
    boot_prefixes folder holds our loader (an entry older than that line), is a copy of the entry; it goes, with the
    files of that switchroot folder.
  * Data: the one top-level folder besides Switch holding deploy's overlay/ is Switch under its former name; and the
    Joy-Con helper's record of the pairing it imported, under its former bundle ID.
  * Preboot: the one top-level folder besides the staging folder holding every cryptex image the spec names.
Each is moved to its new name once, when that name is not taken yet; anything else is left as is.
"""

import os
import re
from pathlib import Path

from tools.install import host

ENTRY_LINE = 'scan_dev_for_extlinux=setexpr.l ios_uboot '
LOADER = 'loader.uboot.bin'
PREFIXES = re.compile(r'^boot_prefixes=/(switchroot/[^/\s]+)/\s*$', re.MULTILINE)
DATA_FOLDER = 'Switch'
PAIRING_RECORD = 'mobile/Library/com.brunostjohn.switch.joycon-pairing.imported.plist'
PAIRING_SUFFIX = '.joycon-pairing.imported.plist'
PREBOOT = '/private/preboot/'


def older_boot_entries(fat_root: Path, ini: str, entry: str) -> list[str]:
    """Return the FAT paths of copies of this boot entry under other names: their folders' files, then their inis."""
    inis, files = [], []
    for path in sorted((Path(fat_root) / 'bootloader/ini').glob('*.ini')):
        relative = 'bootloader/ini/' + path.name
        text = path.read_text(errors='replace') if path.is_file() else ''
        folders = [f for f in PREFIXES.findall(text) if f != entry and (Path(fat_root) / f).is_dir()]
        if relative == ini or not (ENTRY_LINE in text or any((Path(fat_root) / f / LOADER).is_file() for f in folders)):
            continue
        inis.append(relative)
        for folder in folders:
            files += [f'{folder}/{p.name}' for p in sorted((Path(fat_root) / folder).iterdir()) if p.is_file()]
    return files + inis


def remove_older_boot_entries(fat_root: Path, ini: str, entry: str, dry_run: bool) -> list[str]:
    """Remove the copies of this boot entry under other names (their folders too, once empty); return their paths."""
    removed = older_boot_entries(fat_root, ini, entry)
    if dry_run:
        return removed
    for relative in removed:
        (Path(fat_root) / relative).unlink()
    for folder in sorted({str(Path(r).parent) for r in removed if r.startswith('switchroot/')}):
        path = Path(fat_root) / folder
        if path.is_dir() and not any(path.iterdir()):
            path.rmdir()
            removed.append(folder + '/')
    return removed


def _has_overlay(folder: Path) -> bool:
    # Data mounted with owners keeps root's 0700 folders (audit/, ...) closed to the user: not one of ours
    try:
        return folder.is_dir() and not folder.is_symlink() and (folder / 'overlay').is_dir()
    except PermissionError:
        return False


def _older_data_folder(data_root: Path) -> Path | None:
    found = [p for p in Path(data_root).iterdir() if p.name != DATA_FOLDER and _has_overlay(p)]
    return found[0] if len(found) == 1 else None


def _older_pairing_record(data_root: Path) -> Path | None:
    folder = (Path(data_root) / PAIRING_RECORD).parent
    found = (
        [p for p in folder.glob('*' + PAIRING_SUFFIX) if p.name != Path(PAIRING_RECORD).name] if folder.is_dir() else []
    )
    return found[0] if len(found) == 1 else None


def data_folder(data_root: Path) -> Path:
    """Return the Data volume's own folder (/private/var/Switch), or its former one while only that exists."""
    new = Path(data_root) / DATA_FOLDER
    return new if new.exists() else _older_data_folder(data_root) or new


def _move(root: Path, old: Path | None, new: str, log: host.Log, dry_run: bool) -> list[str]:
    if old is None or not old.exists():
        return []
    if (Path(root) / new).exists():
        log(f'{old.relative_to(root)} left as is: {new} exists')
        return []
    if not dry_run:
        os.rename(old, Path(root) / new)
    log(f'{old.relative_to(root)} {"to move to" if dry_run else "moved to"} {new}')
    return [str(old.relative_to(root))]


def migrate_data(data_root: Path, log: host.Log, dry_run: bool = False) -> list[str]:
    """Move the Data volume's folder and the Joy-Con pairing record to their new names; return what moved."""
    moved = _move(data_root, _older_data_folder(data_root), DATA_FOLDER, log, dry_run)
    return moved + _move(data_root, _older_pairing_record(data_root), PAIRING_RECORD, log, dry_run)


def migrate_preboot(preboot_root: Path, spec: dict, log: host.Log, dry_run: bool = False) -> list[str]:
    """Move the cryptex images' folder to the staging folder the spec names; return what moved."""
    paths = [c['path'][len(PREBOOT) :] for c in spec['cryptexes']]
    staging = {str(Path(p).parent) for p in paths}
    if len(staging) != 1:
        return []
    new = staging.pop()
    images = {Path(p).name for p in paths}
    found = [
        p
        for p in Path(preboot_root).iterdir()
        if p.name != new and p.is_dir() and not p.is_symlink() and images <= {c.name for c in p.iterdir()}
    ]
    return _move(preboot_root, found[0] if len(found) == 1 else None, new, log, dry_run)
