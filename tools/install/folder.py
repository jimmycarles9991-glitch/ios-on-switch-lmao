"""The folder //:image makes (out/sd), and how //:image_to_card puts it on the SD card.

    switchroot/ios/NAME.SII.001 ...    one image per iOS version (IOS17, IOS27): manifest, boot files, container
    bootloader/ini/ios.ini             the hekate entry, and its icon bootloader/res/icon_ios_hue.bmp
    bootloader/payloads/install-ios.bin

The boot files hekate starts (switchroot/ios/bl31.bin, ...) are never in the folder: install-ios.bin copies the
installed image's own there, and deploy writes them for the version it was built for. Copying an image to the card
writes only its own NAME.SII.* (the parts that differ, by the manifests' chunk hashes), removes its stale parts and the
image from before .SII files (switchroot/ios/install/), and refreshes the payload, ini and icon. Nothing else is read
or written.
"""

import json
import os
import shutil
from pathlib import Path

from tools.common import hashing
from tools.common.files import replacing
from tools.install import card as cards, host, manifest, progress, renamed

SII_DIR = cards.BOOT_ENTRY
LEGACY = cards.BOOT_ENTRY + '/install'
PAYLOAD = 'bootloader/payloads/install-ios.bin'
MARKER = '.switch-ios-image'
SHARED = (cards.INI, cards.ICON, PAYLOAD)
PART = cards.INSTALL_PART
COPY_CHUNK = 64 << 20


def image_name(pins: dict) -> str:
    return 'IOS{}'.format(pins['version'].split('.')[0])


def _allowed(relative: str) -> bool:
    directory, _, name = relative.rpartition('/')
    return relative in SHARED or (directory == SII_DIR and PART.fullmatch(name) is not None)


def records(out: Path) -> dict[str, dict]:
    """Return {NAME: what //:image recorded of it} for each image the folder holds."""
    try:
        saved = json.loads((Path(out) / MARKER).read_text())
    except (OSError, ValueError):
        return {}
    return saved.get('images', {}) if isinstance(saved, dict) else {}


def record(out: Path, name: str, entry: dict | None) -> None:
    saved = records(out)
    if entry is None:
        saved.pop(name, None)
    else:
        saved[name] = entry
    (Path(out) / MARKER).write_text(json.dumps({'images': saved}, indent=2) + '\n')


def prepare(out: str | Path) -> Path:
    """Return `out` ready for an image: a new folder, or one an earlier run made, whose images all stay.

    Refuses any other directory. A folder from before .SII files loses its image (switchroot/ios/install/).
    """
    out = Path(out)
    if out.exists() and any(out.iterdir()) and not (out / MARKER).is_file():
        raise host.HostError(f'{out} is not empty and was not made by //:image: give --out a new folder')
    if (out / LEGACY).exists():
        shutil.rmtree(out / LEGACY)
    (out / SII_DIR).mkdir(parents=True, exist_ok=True)
    if not (out / MARKER).is_file():
        (out / MARKER).write_text(json.dumps({'images': {}}) + '\n')
    return out


def add(out: Path, relative: str, source: str | Path) -> None:
    if not _allowed(relative):
        raise host.HostError(f'{relative} is not a path the folder may hold')
    target = Path(out) / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, target)
    target.chmod(0o644)


def files(out: Path) -> list[str]:
    """List what of the folder goes onto the card: the paths it may hold, never what is kept beside them for the Mac."""
    out = Path(out)
    found = (p.relative_to(out).as_posix() for p in out.rglob('*') if p.is_file() and not p.name.startswith('._'))
    return sorted(relative for relative in found if _allowed(relative))


def images(root: Path) -> dict[str, manifest.Manifest | None]:
    """Return {NAME: its manifest, None when unreadable} of the images in `root`'s switchroot/ios."""
    found = {}
    for first in sorted((Path(root) / SII_DIR).glob('*.SII.001')):
        name = first.name[: -len('.SII.001')]
        try:
            with open(first, 'rb') as f:
                found[name] = manifest.decode(f.read(manifest.BOOT_OFFSET))
        except (OSError, manifest.ManifestError):
            found[name] = None
    return found


def _same_head(mine: manifest.Manifest, theirs: manifest.Manifest) -> bool:
    return manifest.encode(mine) == manifest.encode(theirs)


def _same_body(mine: manifest.Manifest, theirs: manifest.Manifest, start: int, end: int) -> bool:
    first = max(0, start - manifest.BODY_OFFSET) // manifest.HASH_CHUNK
    last = -(-(end - manifest.BODY_OFFSET) // manifest.HASH_CHUNK)
    return len(theirs.hashes) >= last and mine.hashes[first:last] == theirs.hashes[first:last]


def copy_plan(out: Path, fat_root: Path, name: str) -> tuple[list[tuple[str, int, int]], list[str]]:
    """Return ([(relative, start, end)] to copy, [relative] to remove) for image NAME onto the card.

    A part whose bytes the card already holds (equal size and chunk hashes) is left; when only the head of .001 differs
    (new boot files), only the head is written.
    """
    mine = images(out)[name]
    theirs = images(fat_root).get(name)
    copies, at = [], 0
    for part in mine.parts:
        relative = f'{SII_DIR}/{Path(part.path).name}'
        target = Path(fat_root) / relative
        start, end = at, at + part.bytes
        at = end
        same_file = theirs is not None and target.is_file() and target.stat().st_size == part.bytes
        same_body = same_file and _same_body(mine, theirs, start, end)
        if same_body and (start or _same_head(mine, theirs)):
            continue
        copies.append((relative, 0, manifest.BODY_OFFSET if same_body else part.bytes))
    keep = {Path(p.path).name for p in mine.parts}
    stale = [
        f'{SII_DIR}/{p.name}' for p in sorted((Path(fat_root) / SII_DIR).glob(f'{name}.SII.*')) if p.name not in keep
    ]
    legacy = Path(fat_root) / LEGACY
    stale += [f'{LEGACY}/{p.name}' for p in sorted(legacy.iterdir()) if p.is_file()] if legacy.is_dir() else []
    return copies, stale


def _copy_range(source: Path, target: Path, start: int, end: int) -> None:
    if start == 0 and end == source.stat().st_size:
        with replacing(target) as partial:
            _copy_bytes(source, partial, start, end, 'wb')
    else:
        _copy_bytes(source, target, start, end, 'r+b')


def _copy_bytes(source: Path, target: Path, start: int, end: int, mode: str) -> None:
    with open(source, 'rb') as src, open(target, mode) as dst:
        src.seek(start)
        dst.seek(start)
        while start < end:
            data = src.read(min(COPY_CHUNK, end - start))
            dst.write(data)
            start += len(data)
            progress.advance(len(data))
        dst.flush()
        os.fsync(dst.fileno())


def merge(out: Path, fat_root: Path, names: list[str], log: host.Log) -> tuple[list[str], list[str]]:
    """Put images NAMES and the payload, ini and icon from `out` onto a mounted FAT; return (written, removed)."""
    out, fat_root = Path(out), Path(fat_root)
    written, removed = [], []
    with progress.step('card: copying', copy_bytes(out, fat_root, names)):
        for name in names:
            copies, stale = copy_plan(out, fat_root, name)
            for relative in stale:
                (fat_root / relative).unlink()
                removed.append(relative)
            for relative, start, end in copies:
                progress.note(relative)
                (fat_root / relative).parent.mkdir(parents=True, exist_ok=True)
                _copy_range(out / relative, fat_root / relative, start, end)
                written.append(relative if end != manifest.BODY_OFFSET or start else relative + ' (head)')
        for relative in shared_plan(out, fat_root):
            progress.note(relative)
            source, target = out / relative, fat_root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            _copy_range(source, target, 0, source.stat().st_size)
            written.append(relative)
    if (fat_root / LEGACY).is_dir() and not any((fat_root / LEGACY).iterdir()):
        (fat_root / LEGACY).rmdir()
    removed += renamed.remove_older_boot_entries(fat_root, cards.INI, cards.BOOT_ENTRY, False)
    host.run('sync')
    log(f'card: {len(written)} written, {len(removed)} removed: {", ".join(written + removed) or "nothing"}')
    return written, removed


def shared_plan(out: Path, fat_root: Path) -> list[str]:
    """Return the shared files (install-ios.bin, the ini, the icon) the FAT lacks or holds other bytes of."""
    differ = []
    for relative in SHARED:
        source, target = Path(out) / relative, Path(fat_root) / relative
        if source.is_file() and not (target.is_file() and hashing.sha256_file(target) == hashing.sha256_file(source)):
            differ.append(relative)
    return differ


def copy_bytes(out: Path, fat_root: Path, names: list[str]) -> int:
    images = sum(end - start for name in names for _, start, end in copy_plan(out, fat_root, name)[0])
    return images + sum((Path(out) / r).stat().st_size for r in shared_plan(out, fat_root))
