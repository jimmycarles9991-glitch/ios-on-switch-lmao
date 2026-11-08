"""Disk images attached and volumes mounted only while a step uses them, whatever ends the step.

attached() and mounted() undo themselves on every exit; under interrupts() SIGTERM and SIGHUP end a run the way ^C
does, so their cleanup runs then too. A run that died anyway (SIGKILL, a power cut) leaves attachments behind:
sweep() detaches every image still attached from below a directory before the next run uses it, and removes the
partial files a run writes before renaming them into place.
"""

import contextlib
import os
import re
import shutil
import signal
import tempfile
import time
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path
from types import FrameType

from tools.common import files
from tools.install import host

UNMOUNT_TRIES = 5
MOUNT_TRIES = 3

APFS_VOLUME = '41504653-0000-11AA-AA11-00306543ECAC'
# Partial files are files.partial()'s NAME.partial; runs before tools/common/files named them .partial-NAME.
PARTIALS = ('*' + files.SUFFIX, '.partial-*')
FSEVENTS = '.fseventsd'
ATTACH = ('-nomount', '-noverify', '-noautofsck')
RAW = ('-imagekey', 'diskimage-class=CRawDiskImage')


@dataclass(frozen=True)
class Attachment:
    whole: str
    volumes: list[str]


def _entities(image: Path, writable: bool, raw: bool, log: host.Log) -> list[dict]:
    mode = ('-readwrite',) if writable else ('-readonly',)
    args = ['attach', '-plist', *mode, *(RAW if raw else ()), *ATTACH, str(image)]
    return host.plist('hdiutil', *args, log=log)['system-entities']


@contextlib.contextmanager
def attached(image: Path, log: host.Log, writable: bool = True, raw: bool = False) -> Iterator[Attachment]:
    """Attach `image` without mounting it; `raw` for a file of bytes with no image header (a cryptex, our scratch)."""
    entities = _entities(Path(image), writable, raw, log)
    disks = [e['dev-entry'][5:] for e in entities if re.fullmatch(r'/dev/disk\d+', e.get('dev-entry', ''))]
    if not disks:
        raise host.HostError(f'hdiutil attach gave no disk for {image}')
    volumes = [
        e['dev-entry'][5:]
        for e in entities
        if e.get('content-hint', '').upper() == APFS_VOLUME and re.fullmatch(r'/dev/disk\d+s\d+', e['dev-entry'])
    ]
    try:
        yield Attachment(disks[0], volumes)
    finally:
        host.run('hdiutil', 'detach', disks[0], log=log, check=False)


@contextlib.contextmanager
def mounted(device: str, log: host.Log, owners: bool, read_only: bool = False) -> Iterator[Path]:
    """Mount a volume at a private mount point, hidden from the Finder and Spotlight.

    `owners` shows the files' owners as they are; without, every file reads and writes as the user and new files are
    the user's (owners.py gives them theirs afterwards). On a writable volume fseventsd is told to log nothing
    (quiet()) and its directory goes before unmounting (settle()).
    """
    with tempfile.TemporaryDirectory(prefix='switch-') as directory:
        options = 'nobrowse,' + ('owners' if owners else 'noowners')
        mode = ['readOnly'] if read_only else []
        mount_at(device, directory, [*mode, '-mountOptions', options], log)
        try:
            if not read_only:
                quiet(Path(directory))
            yield Path(directory)
        finally:
            if not read_only:
                settle(Path(directory))
            unmount(device, log, force=True)


def mount_at(device: str, directory: str, args: list[str], log: host.Log) -> None:
    """`diskutil mount <args> -mountPoint directory device`, refused unless the volume really is at `directory`."""
    where = None
    for attempt in range(MOUNT_TRIES):
        host.run(
            'diskutil', 'mount', *args, '-mountPoint', directory, device, log=log, check=attempt + 1 == MOUNT_TRIES
        )
        where = host.mount_point(device)
        if where and os.path.realpath(where) == os.path.realpath(directory):
            return
        # macOS can mount the volume itself (after a partition table write, or a second copy of a volume UUID it has
        # seen) between our check and our mount; whatever reads `directory` would then read an empty folder.
        if where:
            unmount(device, log)
    raise host.HostError(f'{device} would not mount at {directory} (macOS has it at {where or "nowhere"})')


def unmount(device: str, log: host.Log, force: bool = False) -> None:
    """Unmount a volume, trying again: Spotlight, fseventsd or FSKit hold one busy for a moment after it was written.

    `force` (the builder's own scratch volumes only) forces the last try.
    """
    for _ in range(UNMOUNT_TRIES):
        host.run('diskutil', 'unmount', device, log=log, check=False)
        if not host.mount_point(device):
            return
        time.sleep(1)
    if force:
        host.run('diskutil', 'unmount', 'force', device, log=log, check=False)
        if not host.mount_point(device):
            return
    log(f'{device} stays mounted')


def quiet(root: Path) -> None:
    """Make fseventsd log nothing on the volume mounted at `root` (its no_log marker).

    fseventsd makes .fseventsd on a volume when it is mounted, at a moment of its own, and writes its log there when
    the volume is unmounted: files that would land after their owners were recorded. Removing the directory alone
    races with its making.
    """
    events = root / FSEVENTS
    events.mkdir(exist_ok=True)
    (events / 'no_log').touch()


def settle(root: Path) -> None:
    """Remove .fseventsd: the device makes its own, and a cryptex has none. With no_log nothing writes it again."""
    shutil.rmtree(root / FSEVENTS, ignore_errors=True)


def sweep(directory: Path, log: host.Log) -> list[str]:
    """Detach every image attached from below `directory` and delete its partial files; return what was done."""
    root = Path(directory).resolve()
    done = []
    for item in host.plist('hdiutil', 'info', '-plist').get('images', []):
        path = Path(item.get('image-path', ''))
        if not path.is_relative_to(root):
            continue
        entities = item.get('system-entities', [])
        whole = [e['dev-entry'] for e in entities if re.fullmatch(r'/dev/disk\d+', e.get('dev-entry', ''))]
        if whole:
            host.run('hdiutil', 'detach', '-force', whole[0], log=log, check=False)
            done.append(f'detached {path.name} ({whole[0]})')
    for leftover in sorted({p for pattern in PARTIALS for p in root.rglob(pattern)}) if root.is_dir() else []:
        if leftover.is_dir() and not leftover.is_symlink():
            shutil.rmtree(leftover)
        else:
            leftover.unlink(missing_ok=True)
        done.append(f'removed {leftover.name}')
    for line in done:
        log(f'cleanup: {line}')
    return done


def _raise_interrupt(signum: int, _frame: FrameType | None) -> None:
    raise KeyboardInterrupt(signal.Signals(signum).name)


@contextlib.contextmanager
def interrupts() -> Iterator[None]:
    """Make SIGTERM and SIGHUP end the run as ^C does, unwinding every attached() and mounted() on the way out."""
    caught = (signal.SIGTERM, signal.SIGHUP)
    previous = {s: signal.signal(s, _raise_interrupt) for s in caught}
    try:
        yield
    finally:
        for s, handler in previous.items():
            signal.signal(s, handler)


def free_bytes(path: str | os.PathLike) -> int:
    """Free bytes on the filesystem holding `path`, or its nearest existing parent."""
    path = Path(path)
    while not path.exists():
        path = path.parent
    return shutil.disk_usage(path).free
