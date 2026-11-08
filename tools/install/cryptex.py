"""A cryptex with its pretranslated delta in place, sealed, for Preboot (iOS 27), made as the user.

The decrypted cryptex is a raw image of one APFS volume (UDRW, no partition map) that APFS checks block by block
against its seal. Apple's sealed volume cannot be sealed again (apfs_prepare_cryptex fails on it untouched), so the
translated cryptex is a fresh volume of the source's name, written once: every source file the delta leaves alone is
copied with ditto (compression kept), each translated file goes straight to its path in place of the original, the
source's owners, modes and flags are given back offline (owners.py), and macOS's own sealer seals it. Nothing is
rewritten, so no freed blocks leave holes, and the volume is sized from what its files take: the cryptex is about the
size of its files, and room.fit() cuts what is left when macOS still takes the result. newfs_apfs picks a random volume
UUID, so the volume is given the one the IPSW's pins name for the cryptex it replaces, the one the kernel finds it by
(SWITCH_*_CRYPTEX_UUID, //kernel:ipsw_volumes_header); the seal does not cover the volume superblock.
"""

import ctypes
import os
import shutil
import tempfile
import uuid
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path

from tools.common import files, hashing
from tools.install import apfs, apfs_tree, compose, delta, disks, host, owners, progress, room

SEALER = '/System/Library/Filesystems/apfs.fs/Contents/Resources/apfs_prepare_cryptex'
# The sealer writes every node of the tree again, hashed, before the old ones are freed, and adds file-info records
# beside the files' extents: room for two trees.
SEAL_TREE_COPIES = 2
XATTR_NOFOLLOW = 0x1
LIBC = ctypes.CDLL(None, use_errno=True)


def _xattrs(path: Path) -> list[str]:
    """Return the extended attributes listxattr shows (a compressed file's decmpfs and resource fork stay hidden)."""
    size = LIBC.listxattr(os.fsencode(path), None, 0, XATTR_NOFOLLOW)
    if size <= 0:
        return []
    names = ctypes.create_string_buffer(size)
    size = LIBC.listxattr(os.fsencode(path), names, size, XATTR_NOFOLLOW)
    return [n.decode() for n in names.raw[:size].split(b'\0') if n]


class Plan:
    """The source's files, which of them the delta replaces, and the room the new volume needs."""

    def __init__(self, source: Path, entries: list[dict]) -> None:
        self.paths = owners.walk(source)
        inode_of = {path: info.st_ino for path, info in self.paths}
        replacements: dict[int, dict] = {}
        for e in entries:
            ino = inode_of.get(source / e['path'].lstrip('/'))
            if ino is None:
                raise delta.DeltaError(f'{e["path"]} is not in the source')
            first = replacements.setdefault(ino, e)
            if first['sha256_out'] != e['sha256_out']:
                raise delta.DeltaError(f'{first["path"]} and {e["path"]} are one file translated two ways')
        self.replacements = replacements
        self.replaced = [(path, info) for path, info in self.paths if info.st_ino in replacements]
        for path, _ in self.replaced:
            if _xattrs(path):
                raise delta.DeltaError(f'{path} has extended attributes the translated file would not carry')
        originals = {info.st_ino: info.st_blocks * 512 for _, info in self.replaced}
        added = sum(-(-os.path.getsize(e['source']) // room.BLOCK) * room.BLOCK for e in replacements.values())
        self.bytes = room.used_bytes(source) - sum(originals.values()) + added

    def kept(self, source: Path) -> Iterator[str]:
        """Yield every source path the delta leaves alone as `lsbom -s` lists it ('.', './usr/...')."""
        for path, info in self.paths:
            if info.st_ino not in self.replacements:
                relative = path.relative_to(source).as_posix()
                if '\n' in relative:
                    raise delta.DeltaError(f'{relative!r}: a path with a newline cannot go in a bill of materials')
                yield '.' if relative == '.' else './' + relative


def _copy_kept(plan: Plan, source: Path, target: Path, log: host.Log, what: str) -> None:
    with tempfile.TemporaryDirectory(prefix='switch-bom-') as work:
        listing, bom = Path(work) / 'kept.txt', Path(work) / 'kept.bom'
        listing.write_text(''.join(line + '\n' for line in plan.kept(source)))
        host.run('mkbom', '-s', '-i', listing, bom, log=log)
        start, whole = room.used_bytes(target), room.used_bytes(source)
        with progress.step(f'{what}: copying the cryptex', whole, probe=lambda: room.used_bytes(target) - start):
            host.run('ditto', '-bom', bom, '--preserveHFSCompression', source, target, log=log)


def _place(plan: Plan, source: Path, target: Path, what: str) -> None:
    """Write each translated file once at its first path, link its other paths, with the original's times."""
    placed: dict[int, Path] = {}
    total = sum(os.path.getsize(e['source']) for e in plan.replacements.values())
    with progress.step(f'{what}: writing the translated files', 2 * total):
        for path, info in plan.replaced:
            at = target / path.relative_to(source)
            at.unlink(missing_ok=True)
            if info.st_ino in placed:
                os.link(placed[info.st_ino], at)
                continue
            e = plan.replacements[info.st_ino]
            shutil.copyfile(e['source'], at)
            progress.advance(os.path.getsize(at))
            if hashing.sha256_file(at, progress.advance) != e['sha256_out']:
                raise delta.DeltaError(f'{e["path"]} does not hold the translated bytes after the write')
            os.utime(at, ns=(info.st_atime_ns, info.st_mtime_ns), follow_symlinks=False)
            placed[info.st_ino] = at


def _fill(source: str, entries: list[dict], image: Path, log: host.Log, what: str) -> dict[int, owners.Owner]:
    """Make `image` a volume holding the translated files; return their owners by inode."""
    journal = owners.Journal()
    with disks.attached(Path(source), log, writable=False) as src_image:
        if len(src_image.volumes) != 1:
            raise host.HostError(f'{source}: expected one APFS volume, got {src_image.volumes}')
        volume = src_image.volumes[0]
        name = (host.disk_info(volume) or {}).get('VolumeName') or what
        with disks.mounted(volume, log, owners=True, read_only=True) as src:
            journal.copy(src)
        with disks.mounted(volume, log, owners=False, read_only=True) as src:
            plan = Plan(src, entries)
            free = disks.free_bytes(image.parent)
            if free < plan.bytes:
                have, needed = progress.size(free), progress.size(plan.bytes)
                raise host.HostError(f'{what}: {image.parent} has {have} free; the cryptex takes {needed}')
            room.blank(image, plan.bytes, name, log)

            def write(root: Path) -> dict[int, owners.Owner]:
                _copy_kept(plan, src, root, log, what)
                _place(plan, src, root, what)
                disks.settle(root)
                return journal.resolve(root)

            return room.with_room(image, lambda _root: plan.bytes, write, log, what)


def _tree_bytes(image: Path) -> int:
    fd = os.open(image, os.O_RDONLY)
    try:
        return apfs_tree.Volume(fd).tree_bytes()
    finally:
        os.close(fd)


def _give_volume_uuid(fd: int, volume_uuid: uuid.UUID, log: host.Log, what: str) -> None:
    container = apfs.Container(fd)
    found = container.volumes()
    if len(found) != 1:
        raise host.HostError(f'{what}: expected one volume, found {found}')
    current = found[0][0]
    if current != volume_uuid:
        container.set_volume_uuids({current: volume_uuid})
    log(f'{what}: volume UUID {volume_uuid}')


def _check_sealed(
    image: Path, by_inode: dict[int, owners.Owner], volume_uuid: uuid.UUID, log: host.Log, what: str
) -> None:
    with disks.attached(image, log, writable=False, raw=True) as target:
        if len(target.volumes) != 1:
            raise host.HostError(f'{what}: macOS attaches no volume from {image.name}')
        volume = target.volumes[0]
        compose.fsck(volume.rpartition('s')[0], what, log)
        info = host.disk_info(volume) or {}
        if not info.get('Sealed'):
            raise host.HostError(f'{what}: the volume is not sealed')
        if info.get('VolumeUUID', '').upper() != str(volume_uuid).upper():
            raise host.HostError(f'{what}: the volume is {info.get("VolumeUUID")}, not {volume_uuid}')
        with disks.mounted(volume, log, owners=True, read_only=True) as root:
            owners.check(root, by_inode)


@dataclass(frozen=True)
class Job:
    """`source`'s volume with `entries` in place and `volume_uuid`, written to `out` (fitted with `apfs_tool`)."""

    source: str
    entries: list[dict]
    out: Path
    apfs_tool: str
    what: str
    volume_uuid: uuid.UUID


def rebuild(job: Job, log: host.Log) -> None:
    """Write the job's cryptex: its owners given back, sealed, fitted, made beside `out` and renamed into place."""
    with files.replacing(job.out) as image:
        fitted = _rebuild(job, image, log)
    log(f'{job.what}: translated, sealed and fitted to {progress.size(fitted)} as {job.out}')


def _rebuild(job: Job, image: Path, log: host.Log) -> int:
    source, entries, what = job.source, job.entries, job.what
    by_inode = _fill(source, entries, image, log, what)
    fd = os.open(image, os.O_RDWR)
    try:
        log(f'{what}: {owners.apply(fd, 0, None, by_inode)} inodes given their owners')
        _give_volume_uuid(fd, job.volume_uuid, log, what)
    finally:
        os.close(fd)
    tree = _tree_bytes(image)
    room.make_room(image, lambda _root: SEAL_TREE_COPIES * tree, log, what)
    # The sealer mounts the container itself: it takes the volume's node, attached and not mounted.
    with disks.attached(image, log, raw=True) as target, progress.step(f'{what}: sealing', 100, progress.PERCENT):
        host.watch(SEALER, '-p', '/dev/' + target.volumes[0], parse=progress.sealer, log=log)
    _check_sealed(image, by_inode, job.volume_uuid, log, what)
    return room.fit(
        image, job.apfs_tool, lambda clone: _check_sealed(clone, by_inode, job.volume_uuid, log, what), log, what
    )
