"""iOS 27's user space, translated to plain arm64 (//translate/arm64e/pretranslate), placed while the image is composed.

Bazel makes each role's delta (delta.py) from its mounted volume (install/translated.bzl). The SystemOS cryptex goes
last: the prebuilt loaders in its dyld cache check the binaries of every volume by cdhash, so its run takes the cdhash
pairs the System volume's and the App cryptex's runs wrote, or each launch falls back to a slower loader.

The System volume stays sealed as Apple shipped it. Its translated files go to Data under Switch/pretranslated, at
their System paths, its own tree because a deploy replaces Switch/overlay whole; the kernel shows them at those paths
from the tree's own index (overlay_index.py), made from Switch/pretranslated.json and the files' ids once they are
placed.
"""

import json
import os
import shutil
import stat
from collections.abc import Callable
from pathlib import Path

from tools.common import files, hashing
from tools.install import delta, host, overlay_index, owners as ownership, progress, renamed

OVERLAY = renamed.DATA_FOLDER + '/pretranslated'
OVERLAY_MANIFEST = OVERLAY + '.json'
OVERLAY_INDEX = OVERLAY + '/' + overlay_index.NAME
OVERLAY_ORIGINALS = OVERLAY + '/' + overlay_index.ORIGINALS_NAME
# installd verifies each staged system app against Apple's detached signature (/System/Library/AppSignatures), whose
# CodeDirectory hashes the original executable's pages: Data's copies keep Apple's bytes, the translations go to the
# tree, root's (the System volume gives them mobile's owner), and the kernel finds them by the original's CDHash.
STAGED_APPS = '/private/var/staged_system_apps/'
COMPRESSED = 0x20

ORDER = ('os', 'app_cryptex', 'system_cryptex')
SystemStat = Callable[[str], os.stat_result]
# What the boot package needs of a run (trust modules, the device tree's list of translated cache cdhashes).
KEPT = ('{role}.trustcache', '{role}.cdhash-pairs', 'a64x-cache-pretranslated.bin')


def _cdhash(cdhash: list[str]) -> bytes:
    return bytes.fromhex(cdhash[0]) if cdhash else b'\x00' * overlay_index.CDHASH


def _owner(e: dict, original: os.stat_result) -> tuple[int, int]:
    return (0, 0) if e['path'].startswith(STAGED_APPS) else (original.st_uid, original.st_gid)


def _place(root: Path, e: dict, original: os.stat_result) -> Path:
    """Copy e's translated file to its System path below root, with the System file's mode."""
    target = root / e['path'].lstrip('/')
    target.parent.mkdir(parents=True, exist_ok=True)
    with files.replacing(target) as partial:
        shutil.copyfile(e['source'], partial)
        progress.advance(partial.stat().st_size)
        if hashing.sha256_file(partial, progress.advance) != e['sha256_out']:
            raise host.HostError(f'{e["path"]}: the translated file does not match its manifest')
    target.chmod(original.st_mode & 0o7777)
    return target


class Deltas:
    """iOS 27's deltas, made by Bazel (install/translated.bzl), and what of them the boot package keeps."""

    def __init__(self, dirs: dict[str, str | Path], kept: Path, log: host.Log) -> None:
        """`dirs`: {role: the role's delta directory}; `kept`: where keep() puts the files the boot package pins."""
        unknown = set(dirs) - set(ORDER)
        if unknown:
            raise host.HostError(f'unknown pretranslate roles: {sorted(unknown)}')
        self.dirs, self.kept, self.log = {r: Path(d) for r, d in dirs.items()}, Path(kept), log

    def __contains__(self, role: str) -> bool:
        return role in self.dirs

    def entries(self, role: str) -> list[dict]:
        """Return the role's delta, ready for delta.apply; its files are Bazel's and are only read."""
        entries = delta.load(self.dirs[role] / f'{role}.manifest.json')
        self.log(f'{role}: {len(entries)} files translated')
        return entries

    def keep(self) -> None:
        if self.kept.exists():
            shutil.rmtree(self.kept)
        self.kept.mkdir(parents=True)
        for role, directory in sorted(self.dirs.items()):
            for name in KEPT:
                path = directory / name.format(role=role)
                if path.is_file():
                    shutil.copyfile(path, self.kept / path.name)

    def artifacts(self) -> list[Path]:
        return sorted(self.kept.iterdir()) if self.kept.is_dir() else []

    def check_pins(self, pins: dict[str, str]) -> None:
        """Compare the kept files with the ones the boot package was built with ({name: sha256}).

        A translation that made other trust modules or another cache list would not run on the kernel.
        """
        kept = {p.name: p for p in self.artifacts()}
        wrong = [
            name
            for name, digest in sorted(pins.items())
            if name not in kept or hashing.sha256_file(kept[name]) != digest
        ]
        if wrong:
            raise host.HostError(
                'this translation differs from the pinned one the boot package carries: {}'.format(', '.join(wrong))
            )
        self.log(f'pretranslate: {len(pins)} files match the pins')

    def _originals(self, translated: list[bytes]) -> list[tuple[bytes, bytes]]:
        """Return (original, translation) for each translated CDHash, from the os role's pairs."""
        pairs = (self.dirs['os'] / 'os.cdhash-pairs').read_text().split('\n')
        original_of = {bytes.fromhex(new): bytes.fromhex(old) for old, new in (line.split() for line in pairs if line)}
        missing = [cdhash.hex() for cdhash in translated if cdhash not in original_of]
        if missing:
            raise host.HostError(f'staged system app translations without an original CDHash: {missing[:3]}')
        return [(original_of[cdhash], cdhash) for cdhash in translated]

    def _write_tables(self, data: Path, ids: list, directories: list[Path]) -> list[Path]:
        """Write the kernel's index of the tree and, for staged system apps, .originals; return the files written."""
        staged = [cdhash for path, _, _, cdhash in ids if path.startswith(STAGED_APPS)]
        tables = [data / OVERLAY_INDEX]
        tables[0].write_bytes(overlay_index.encode(ids, [os.lstat(d).st_ino for d in directories]))
        if staged:
            tables.append(data / OVERLAY_ORIGINALS)
            tables[1].write_bytes(overlay_index.encode_originals(self._originals(staged)))
        for table in tables:
            table.chmod(0o644)
        return tables

    def overlay_system(
        self, data: Path, system: SystemStat, entries: list[dict], owners: ownership.Owners | None
    ) -> dict:
        """Place the System volume's translated files on Data (OVERLAY); return their manifest.

        `system(path)` is the System file's lstat, from a mount that shows its owners. Each translated file gets its
        System file's owner, mode and flags, then the manifest and the kernel's index made from it go beside them; the
        manifest is also kept for the boot package. Every file and directory of the tree ends root's and
        system-immutable: the device's kernel refuses any change to the tree anyway, the flag is a second layer.

        Hard links of one System file get one Data file, placed at the first of their paths, and an index entry for
        each path. A link on Data would give the kernel's redirect a Data file whose parent changes with each lookup.
        The staged system apps' translations also get .originals, from the role's CDHash pairs.
        """
        root = Path(data) / OVERLAY
        files, ids, placed = [], [], []
        by_inode = {}
        work = 2 * sum(e['source'].stat().st_size for e in entries)
        with progress.step('System: placing the translated files on Data', work):
            for e in entries:
                original = system(e['path'])
                if not stat.S_ISREG(original.st_mode):
                    raise host.HostError(f'{e["path"]}: not a regular file on the System volume')
                if original.st_ino in by_inode:
                    first, data_id, record = by_inode[original.st_ino]
                    if (first['sha256_out'], first.get('cdhash')) != (e['sha256_out'], e.get('cdhash')):
                        raise host.HostError(f'{first["path"]} and {e["path"]} are one System file translated two ways')
                    progress.advance(2 * e['source'].stat().st_size)
                else:
                    target = _place(root, e, original)
                    placed.append((target, original, e))
                    data_id = os.lstat(target).st_ino
                    record = {
                        'trust': True,
                        'mode': f'{original.st_mode & 0o7777:04o}',
                        'sha256': e['sha256_out'],
                        'cdhash': e.get('cdhash', []),
                    }
                    by_inode[original.st_ino] = (e, data_id, record)
                ids.append((e['path'], original.st_ino, data_id, _cdhash(e.get('cdhash') or [])))
                files.append({'path': e['path'], **record})
        manifest = {'files': files}
        text = json.dumps(manifest, indent=1) + '\n'
        root.mkdir(parents=True, exist_ok=True)
        (Path(data) / OVERLAY_MANIFEST).write_text(text)
        self.kept.mkdir(parents=True, exist_ok=True)
        (self.kept / 'os.overlay.json').write_text(text)
        directories = [root, *(p for p in root.rglob('*') if p.is_dir())]
        for directory in directories:
            directory.chmod(0o755)
        tables = self._write_tables(Path(data), ids, directories)
        if owners:
            for target, original, e in placed:
                flags = (original.st_flags & ~COMPRESSED) | stat.SF_IMMUTABLE
                owners.set(target, *_owner(e, original), original.st_mode & 0o7777, flags)
            for table in tables:
                owners.set(table, 0, 0, 0o644, stat.SF_IMMUTABLE)
            for directory in reversed(directories):
                owners.set(directory, 0, 0, 0o755, stat.SF_IMMUTABLE)
        self.log(f'System: {len(files)} translated files on Data under {OVERLAY}')
        return manifest
