"""A pretranslated delta (//translate/arm64e/pretranslate, iOS 27) applied to a mounted volume.

The manifest's "files" list [{"path": volume path, "file": replacement, relative to the manifest, "sha256_in",
"sha256_out"}]. Each file is rewritten in place, so its inode, owner, mode, flags and other attributes stay; a
compressed file is stored uncompressed afterwards.
"""

import json
import os
from pathlib import Path

from tools.common import hashing
from tools.install import host, progress

COMPRESSED = 0x20  # UF_COMPRESSED: decmpfs, which a write in place drops
COPY_CHUNK = 8 << 20


class DeltaError(host.HostError):
    pass


def load(manifest: Path) -> list[dict]:
    manifest = Path(manifest)
    entries = json.loads(manifest.read_text())['files']
    for e in entries:
        if not e['path'].startswith('/') or '..' in Path(e['path']).parts:
            raise DeltaError('{}: {!r} is not an absolute volume path'.format(manifest, e['path']))
        e['source'] = manifest.parent / e['file']
    return entries


def entries_under(entries: list[dict], prefix: str) -> list[dict]:
    """Return the entries below `prefix` (e.g. /private/var), paths relative to it: Data holds a copy of that tree."""
    prefix = prefix.rstrip('/') + '/'
    return [dict(e, path='/' + e['path'][len(prefix) :]) for e in entries if e['path'].startswith(prefix)]


def written_bytes(root: Path, entries: list[dict]) -> int:
    """Return the room `entries` below `root` need written whole, in 4 KiB blocks.

    What the files they replace free is not counted, and hard links of one file are written once. The blocks a
    rewritten file frees are not reusable until APFS commits the transaction that freed them, so a volume rewriting
    many files at once can run out with a positive net balance.
    """
    sizes = {}
    for e in entries:
        inode = os.lstat(Path(root) / e['path'].lstrip('/')).st_ino
        sizes[inode] = -(-os.path.getsize(e['source']) // 4096) * 4096
    return sum(sizes.values())


def added_bytes(root: Path, entries: list[dict]) -> int:
    """Return how many bytes applying `entries` below `root` adds to its volume.

    Each replacement is stored uncompressed, in 4 KiB blocks, in place of a file that may be compressed.
    """
    added = 0
    for e in entries:
        target = Path(root) / e['path'].lstrip('/')
        added += -(-os.path.getsize(e['source']) // 4096) * 4096 - os.lstat(target).st_blocks * 512
    return added


def _work_bytes(target: Path, source: Path) -> int:
    """Return what applying one entry reads and writes: the file hashed, its replacement written and hashed."""
    current = target.stat().st_size if target.is_file() else 0
    return current + 2 * (source.stat().st_size if source.is_file() else 0)


def apply(root: Path, entries: list[dict], log: host.Log, what: str, consume: bool = False) -> int:
    """Rewrite each entry's file below `root`.

    A file already holding sha256_out is left alone: a resumed run, or a hard link of a file already rewritten.
    `consume` deletes each replacement once written or found there, so a delta and its volume never both hold the
    bytes.
    """
    targets = [Path(root) / e['path'].lstrip('/') for e in entries]
    work = [_work_bytes(t, e['source']) for t, e in zip(targets, entries, strict=True)]
    changed = 0
    with progress.step(f'{what}: writing the translated files', sum(work)):
        for e, target, size in zip(entries, targets, work, strict=True):
            if not target.is_file() or target.is_symlink():
                raise DeltaError('{}: {} is not a regular file'.format(what, e['path']))
            current = hashing.sha256_file(target, progress.advance)
            if current == e['sha256_out']:
                progress.advance(size - target.stat().st_size)
                if consume:
                    e['source'].unlink(missing_ok=True)
                continue
            if current != e['sha256_in']:
                raise DeltaError('{}: {} is not the file the delta was made from'.format(what, e['path']))
            _rewrite(target, e['source'])
            if hashing.sha256_file(target, progress.advance) != e['sha256_out']:
                raise DeltaError('{}: {} does not hold the translated bytes after the write'.format(what, e['path']))
            if consume:
                e['source'].unlink()
            changed += 1
    log(f'{what}: {changed} of {len(entries)} files translated')
    return changed


def _rewrite(target: Path, source: Path) -> None:
    """Write `source`'s bytes over `target` in place, its flags lifted for the write and put back."""
    flags = os.lstat(target).st_flags
    if flags & ~COMPRESSED:
        os.chflags(target, 0)
    # Opening a compressed file for writing decompresses it in place first (its whole size, transiently); truncating it
    # by path does not
    os.truncate(target, 0)
    with open(source, 'rb') as replacement, open(target, 'r+b') as out:
        while chunk := replacement.read(COPY_CHUNK):
            out.write(chunk)
            progress.advance(len(chunk))
        out.flush()
        os.fsync(out.fileno())
    if flags & ~COMPRESSED:
        os.chflags(target, flags & ~COMPRESSED)
