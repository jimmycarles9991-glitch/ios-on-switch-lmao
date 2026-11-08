"""The Switch SD card as the installer and deploy see it: guards, layout, volumes, and the two places we write.

Guard rails (every command):
  * the argument must name a whole disk (diskN or /dev/diskN), never a partition, volume or path;
  * never the boot disk or a disk holding the running system's APFS container;
  * never an internal non-removable disk;
  * it must look like a Switch card: valid primary and backup GPT, entry #0 "hos_data" a Microsoft basic data (FAT32)
    partition that the hybrid MBR also lists;
  * the target is the one partition target.py selects (any size the kernel takes), Linux-data or APFS typed, not the
    FAT, not emuMMC; no other APFS or iOS entry on the card (the kernel takes the only one);
  * the hybrid MBR may list the target only as the whole partition (hekate lists l4t), never part of it.
Writes on the FAT go only to switchroot/ios/*, bootloader/ini/ios.ini and bootloader/res/icon_ios_hue.bmp (by name), and
remove a copy of this boot
entry under the project's former names (renamed.py) and the icon's former name (FORMER_ICONS); on the iPad volumes only
to the paths the caller names.
"""

import argparse
import contextlib
import hashlib
import os
import re
import shutil
import sys
import tempfile
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path

from tools.common import files, hashing
from tools.install import disks, gpt, host, renamed, target as targets

LINUX_MBR = 0x83
BOOT_ENTRY = 'switchroot/ios'
INI = 'bootloader/ini/ios.ini'
ICON = 'bootloader/res/icon_ios_hue.bmp'
# The menu icon before Nyx tinted it (an untinted tile): hekate never reads it once the ini names ICON.
FORMER_ICONS = ('bootloader/res/icon_ios.bmp',)
# The user's own files in the entry's folder, which no package ships: the loader reads bootargs.txt at boot.
USER_FILES = ('bootargs.txt',)
# The installer's image parts beside the boot files (folder.py writes them, install-ios.bin reads them): never a
# boot package's, so a deploy keeps them.
INSTALL_PART = re.compile(r'([A-Z0-9]+)\.SII\.([0-9]{3})')


class RefusedError(Exception):
    """A guard said no. The message says which and why; nothing was written."""


@dataclass
class Card:
    disk: str  # disk4
    info: dict
    size: int
    image: str = ''  # attached disk image path ("" for real media)
    table: gpt.Table = None  # None until read (raw read needs root on real cards)
    target: gpt.Entry = None
    fat: gpt.Entry = None

    @property
    def raw(self) -> str:
        return '/dev/r' + self.disk

    def slice(self, entry: gpt.Entry) -> str:
        return f'{self.disk}s{entry.index + 1}'


def resolve(argument: str) -> Card:
    """Return the whole disk named by `argument`, after the host-level guards (no raw access needed)."""
    name = argument.removeprefix('/dev/')
    name = name.removeprefix('r')
    if not name.startswith('disk') or not name[4:].isdigit():
        raise RefusedError(f'{argument!r} is not a whole disk (expected diskN)')
    info = host.disk_info(name)
    if not info:
        raise RefusedError(f'no disk {name}')
    if info.get('ParentWholeDisk') != name or not info.get('WholeDisk'):
        raise RefusedError(f'{name} is not a whole disk')
    if info.get('APFSPhysicalStores') or info.get('Content') == 'EF57347C-0000-11AA-AA11-00306543ECAC':
        raise RefusedError(f"{name} is an APFS container (synthesized), not the card: name the card's own disk")
    if info.get('DeviceBlockSize', 512) != 512:
        raise RefusedError('{} has {}-byte blocks; an SD card has 512'.format(name, info.get('DeviceBlockSize')))
    if name in host.boot_disks():
        raise RefusedError(f'{name} holds the running system')
    image = host.images().get(name, '')
    if info.get('Internal') and not info.get('RemovableMedia') and not image:
        raise RefusedError(f'{name} is an internal, non-removable disk')
    if info.get('BusProtocol') == 'Disk Image' and not image:
        raise RefusedError(f'{name} is a disk image hdiutil does not list')
    if not info.get('WritableMedia', True):
        raise RefusedError(f'{name} is read-only media (SD lock switch?)')
    return Card(name, info, int(info['TotalSize']), image)


def confirm(args: argparse.Namespace, disk: str, warning: str) -> None:
    """Have the user type `disk` after `warning` (or pass --confirm=disk), and keep it in args.confirm for sudo."""
    if args.confirm:
        if args.confirm != disk:
            raise RefusedError(f'--confirm={args.confirm} does not name the target {disk}')
        return
    if not sys.stdin.isatty():
        raise RefusedError(f'type-to-confirm needs a terminal (or pass --confirm={disk})')
    answer = input(f'{warning} Type {disk} to continue: ')
    if answer.strip() != disk:
        raise RefusedError('not confirmed')
    args.confirm = disk


def read_table(card: Card) -> gpt.Table:
    try:
        fd = os.open(card.raw, os.O_RDONLY)
    except PermissionError:
        raise host.HostError(f"reading {card.raw}'s partition table needs root (sudo)") from None
    try:
        card.table = gpt.read(fd, card.size)
    except gpt.GptError as error:
        raise RefusedError(f'{card.disk}: {error}') from None
    finally:
        os.close(fd)
    return card.table


def _select(card: Card, entries: list[targets.Candidate], spec: dict, partition: str | None) -> targets.Candidate:
    try:
        return targets.select(entries, spec.get('partition_name', 'iOS'), partition)
    except targets.TargetError as error:
        raise RefusedError(f'{card.disk}: {error}') from None


def check_layout(card: Card, spec: dict, partition: str | None = None) -> gpt.Entry:
    """Find hos_data and the target, or refuse."""
    table = card.table
    entries = table.used
    first = table.entries[0]
    if first.type != gpt.BASIC_DATA or first.name != 'hos_data':
        raise RefusedError(f"{card.disk}: entry #0 is {first.describe()}, not the Switch's hos_data FAT32 partition")
    if not any(p.kind in (0x0B, 0x0C) and p.first == first.first for p in table.mbr_partitions):
        raise RefusedError(f'{card.disk}: the MBR does not list hos_data as FAT32 (no hybrid MBR: not a Switch card?)')
    fat_info = host.disk_info(card.slice(first)) or {}
    if fat_info.get('FilesystemType') not in ('msdos', None) and fat_info.get('FilesystemType'):
        raise RefusedError('{}: hos_data is {}, not FAT'.format(card.disk, fat_info.get('FilesystemType')))
    eligible = [e for e in entries if e.index != first.index and e.type != gpt.EMUMMC]
    chosen = _select(
        card, [targets.Candidate(e.index, e.name, e.type == gpt.APFS, e.bytes) for e in eligible], spec, partition
    )
    target = next(e for e in eligible if e.index == chosen.index)
    if target.type not in (gpt.LINUX_DATA, gpt.APFS):
        raise RefusedError(f'{card.disk}: {target.describe()} is neither Linux data nor APFS')
    for p in table.mbr_partitions:
        if p.kind in (0, 0xEE) or not (target.first <= p.first + p.sectors - 1 and p.first <= target.last):
            continue
        if not (p.kind == LINUX_MBR and p.first == target.first and p.sectors == target.sectors):
            raise RefusedError(f'{card.disk}: the hybrid MBR lists a partition overlapping the target')
    card.target, card.fat = target, first
    return target


@dataclass
class Part:
    """A partition as diskutil reports it (no raw access needed)."""

    index: int
    identifier: str
    name: str
    guid: str
    content: str
    size: int
    filesystem: str


def diskutil_partitions(card: Card) -> list[Part]:
    out = []
    for item in host.partitions(card.disk):
        ident = item['DeviceIdentifier']
        info = host.disk_info(ident) or {}
        out.append(
            Part(
                int(ident.rsplit('s', 1)[1]) - 1,
                ident,
                info.get('IORegistryEntryName', ''),
                (info.get('DiskUUID') or '').upper(),
                info.get('Content', ''),
                int(info.get('TotalSize', 0)),
                info.get('FilesystemType') or '',
            )
        )
    return out


def check_layout_unprivileged(card: Card, spec: dict, partition: str | None = None) -> tuple[Part, Part]:
    """Apply check_layout's guards from diskutil's view (deploy, image sizing, dry runs without root).

    Returns (fat partition, target partition).
    """
    parts = diskutil_partitions(card)
    if (
        not parts
        or parts[0].name != 'hos_data'
        or parts[0].content != 'Microsoft Basic Data'
        or parts[0].filesystem not in ('msdos', '')
    ):
        raise RefusedError(f"{card.disk}: the first partition is not the Switch's hos_data FAT32")
    eligible = [p for p in parts[1:] if p.content.upper() != str(gpt.EMUMMC).upper()]
    chosen = _select(
        card, [targets.Candidate(p.index, p.name, p.content == 'Apple_APFS', p.size) for p in eligible], spec, partition
    )
    return parts[0], next(p for p in eligible if p.index == chosen.index)


def table_snapshot(card: Card) -> bytes:
    """Return the bytes of every partition-table sector (MBR, both headers and arrays) for before/after checks."""
    fd = os.open(card.raw, os.O_RDONLY)
    try:
        t = card.table
        span = t.entry_sectors
        parts = [
            os.pread(fd, (2 + span) * gpt.SECTOR, 0),
            os.pread(fd, span * gpt.SECTOR, t.backup_entries_lba * gpt.SECTOR),
            os.pread(fd, gpt.SECTOR, (t.disk_sectors - 1) * gpt.SECTOR),
        ]
    finally:
        os.close(fd)
    return b''.join(parts)


def changed_outside(card: Card, before: bytes, after: bytes, index: int | None) -> list[int]:
    """Return the offsets at which two table_snapshot()s differ, other than where retyping entry `index` writes.

    Retyping writes that entry in both arrays and the two headers; with `index` None every byte counts.
    """
    if len(after) != len(before):
        raise host.HostError('the partition table changed shape')
    table, span = card.table, card.table.entry_sectors * gpt.SECTOR
    allowed = set()
    if index is not None:
        entry = index * table.entry_size
        allowed |= set(range(gpt.SECTOR, 2 * gpt.SECTOR))
        for base in (2 * gpt.SECTOR, 2 * gpt.SECTOR + span):
            allowed |= set(range(base + entry, base + entry + table.entry_size))
        allowed |= set(range(len(after) - gpt.SECTOR, len(after)))
    return [i for i in range(len(after)) if after[i] != before[i] and i not in allowed]


def volumes(card: Card) -> dict[str, dict]:
    """{uuid: {DeviceIdentifier, Roles, Name, ...}} of the APFS container on the target, {} when none is attached."""
    container = host.container_on(card.slice(card.target))
    if not container:
        return {}
    return {v['APFSVolumeUUID'].upper(): v for v in container.get('Volumes', [])}


@contextlib.contextmanager
def mounted(identifier: str, log: host.Log, read_only: bool = False, owners: bool = False) -> Iterator[Path]:
    """Mount a volume at a private mount point (nobrowse), or reuse where it already is; unmount what we mounted."""
    current = host.mount_point(identifier)
    if current:
        info = host.disk_info(identifier) or {}
        if not read_only and not info.get('WritableVolume'):
            raise host.HostError(f'{identifier} is mounted read-only at {current}')
        if owners or not info.get('GlobalPermissionsEnabled'):
            yield Path(current)
            return
        # macOS's own mount of a card enforces owners, which closes root's 0700 folders to the user
        disks.unmount(identifier, log)
    with tempfile.TemporaryDirectory(prefix='switch-', ignore_cleanup_errors=True) as directory:
        mode = ['readOnly'] if read_only else []
        disks.mount_at(
            identifier, directory, [*mode, '-mountOptions', 'nobrowse' if owners else 'nobrowse,noowners'], log
        )
        try:
            if owners and os.geteuid() == 0:
                host.run('diskutil', 'enableOwnership', directory, log=log)
            yield Path(directory)
        finally:
            host.run('sync')
            disks.unmount(identifier, log)


def boot_entry_files(package: str | Path) -> dict[str, Path]:
    """{relative path on the FAT: source} of the boot package: switchroot/ios/*, the hekate ini and its icon."""
    package = Path(package)
    files = {}
    entry = package / BOOT_ENTRY
    if not entry.is_dir() or not (package / INI).is_file():
        raise host.HostError(f'{package} is not a boot package (no {BOOT_ENTRY}/ or {INI})')
    for path in sorted(entry.iterdir()):
        if path.is_dir():
            raise host.HostError(f'unexpected directory {path} in the boot package')
        if path.name in USER_FILES:
            raise host.HostError(f"{path} is the user's file; no boot package ships it")
        files[f'{BOOT_ENTRY}/{path.name}'] = path
    files[INI] = package / INI
    if (package / ICON).is_file():
        files[ICON] = package / ICON
    return files


def sync_boot_entry(
    fat_root: Path, package: str | Path, log: host.Log, dry_run: bool = False
) -> tuple[list[str], list[str]]:
    """Make switchroot/ios/ and bootloader/ini/ios.ini on the FAT equal the package; nothing else.

    The user's own files (USER_FILES) stay, and a copy of the entry under the project's former names goes (one boot
    entry, replaced in place).
    """
    fat_root = Path(fat_root)
    wanted = boot_entry_files(package)
    changed = _copy_changed(fat_root, wanted, dry_run)
    removed = _remove_stale(fat_root, wanted, dry_run)
    for relative in list(wanted) + removed:  # AppleDouble files, by name only
        double = (fat_root / relative.rstrip('/')).with_name('._' + Path(relative.rstrip('/')).name)
        if double.exists() and not dry_run:
            double.unlink()
    if not dry_run:
        host.run('sync')
    log(
        'boot entry{}: {} {}, {} stale {}, {} unchanged'.format(
            ' (dry run)' if dry_run else '',
            len(changed),
            'to update' if dry_run else 'updated',
            len(removed),
            'to remove' if dry_run else 'removed',
            len(wanted) - len(changed),
        ),
        changed + ['-' + r for r in removed],
    )
    return changed, removed


def _copy_changed(fat_root: Path, wanted: dict[str, Path], dry_run: bool) -> list[str]:
    changed = []
    for relative, source in wanted.items():
        target = fat_root / relative
        if (
            target.is_file()
            and target.stat().st_size == source.stat().st_size
            and hashing.sha256_file(target) == hashing.sha256_file(source)
        ):
            continue
        changed.append(relative)
        if not dry_run:
            target.parent.mkdir(parents=True, exist_ok=True)
            files.copy(source, target)
    return changed


def _remove_stale(fat_root: Path, wanted: dict[str, Path], dry_run: bool) -> list[str]:
    """Remove the entry's files the package no longer has, older entries and former icons; return what went."""
    removed = []
    entry = fat_root / BOOT_ENTRY
    if entry.is_dir():
        for path in sorted(entry.iterdir()):
            relative = f'{BOOT_ENTRY}/{path.name}'
            if (
                relative not in wanted
                and path.is_file()
                and not path.name.startswith('._')
                and path.name not in USER_FILES
                and not INSTALL_PART.fullmatch(path.name)
            ):
                removed.append(relative)
                if not dry_run:
                    path.unlink()
    removed += renamed.remove_older_boot_entries(fat_root, INI, BOOT_ENTRY, dry_run)
    for relative in FORMER_ICONS:
        if (fat_root / relative).is_file():
            removed.append(relative)
            if not dry_run:
                (fat_root / relative).unlink()
    return removed


def tree_digest(root: Path) -> str | None:
    root = Path(root)
    if not root.is_dir():
        return None
    paths = [p for p in sorted(root.rglob('*')) if not p.name.startswith('._')]
    files = [p for p in paths if not p.is_dir()]
    hashes = dict(zip(files, hashing.sha256_files(files), strict=True))
    digest = hashlib.sha256()
    for path in paths:
        relative = path.relative_to(root).as_posix()
        if path in hashes:
            digest.update(f'f {relative} {path.stat().st_mode & 0o777 | 0o200:o} {hashes[path]}\n'.encode())
        else:
            digest.update(b'd ' + relative.encode() + b'\n')
    return digest.hexdigest()


def sync_overlay(data_root: Path, package: str | Path, log: host.Log, dry_run: bool = False) -> bool:
    """<Data>/Switch/overlay := the package's overlay/ (whole tree, like the old deploy script)."""
    source, target = Path(package) / 'overlay', Path(data_root) / renamed.DATA_FOLDER / 'overlay'
    if not source.is_dir():
        log('overlay: the boot package has none; left as is')
        return False
    if tree_digest(source) == tree_digest(target):
        log('overlay: unchanged')
        return False
    if not dry_run:
        if target.exists():
            shutil.rmtree(target)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(source, target, copy_function=shutil.copyfile)
        for path in [source, *source.rglob('*')]:
            copied = target / path.relative_to(source)
            mode = (path.stat().st_mode & 0o777) | 0o200  # Bazel outputs are read-only; keep the x bits
            copied.chmod(mode | (0o755 if path.is_dir() else 0))
            if os.geteuid() == 0:
                os.chown(copied, 0, 0)
        for leftover in target.rglob('._*'):
            leftover.unlink()
        host.run('sync')
    log('overlay: replaced ({} files)'.format(sum(1 for p in source.rglob('*') if p.is_file())))
    return True
