"""File owners for the volumes the image builder writes as the user, given offline once the volume is detached.

Nothing here needs root. While a volume is mounted without owners, every path written gets an entry: copied trees
take theirs from the source mounted with owners (copy()), the rest are named (set()). A path with no entry takes its
directory's owner, as the device's own files would under a process running as root. resolve() turns the entries into
inode numbers while the volume is still mounted; apply() rewrites those inodes in the detached image (apfs_tree.py);
check() mounts it again with owners and compares what macOS reads with what was wanted.
"""

import json
import os
from dataclasses import asdict, dataclass
from pathlib import Path

from tools.install import apfs_tree

COMPRESSED = 0x20  # UF_COMPRESSED: what the volume holds, never what the source had
FIRST_USER_INODE = 16  # below: APFS's reserved inodes (the root directory is 2)
ROOT_INODE = 2


@dataclass(frozen=True)
class Owner:
    """`mode` holds the permission bits and `flags` the BSD flags but UF_COMPRESSED; None keeps the file's own."""

    uid: int
    gid: int
    mode: int | None = None
    flags: int | None = None


class OwnersError(Exception):
    pass


class Journal:
    """Wanted owners by path relative to one volume's root ('' is the root itself).

    `root`, the volume's mount point, lets set() and copy() take paths below it as they are.
    """

    def __init__(self, root: Path | None = None) -> None:
        self.root = Path(root) if root else None
        self.wanted: dict[str, Owner] = {}

    def _key(self, path: str | Path) -> str:
        path = Path(path)
        if path.is_absolute() and self.root:
            path = path.relative_to(self.root)
        text = path.as_posix().strip('/')
        return '' if text == '.' else text

    def set(self, path: str | Path, uid: int, gid: int, mode: int | None = None, flags: int | None = None) -> None:
        self.wanted[self._key(path)] = Owner(uid, gid, mode, flags)

    def copy(self, source: Path, under: str | Path = '') -> int:
        """Record every path below `source` (mounted with owners) as it is there, at `under`; return how many."""
        base = self._key(under)
        count = 0
        for path, info in walk(Path(source)):
            relative = path.relative_to(source).as_posix()
            name = '/'.join(p for p in (base, '' if relative == '.' else relative) if p)
            self.wanted[name] = Owner(info.st_uid, info.st_gid, info.st_mode & 0o7777, info.st_flags & ~COMPRESSED)
            count += 1
        return count

    def resolve(self, root: Path) -> dict[int, Owner]:
        """{inode: owner} of every path below the mounted `root`, unnamed ones taking their directory's owner."""
        if (Path(root) / '.fseventsd').exists():
            raise OwnersError(f"{root} holds macOS's event log, whose files come and go: disks.settle() it first")
        effective: dict[str, Owner] = {}
        by_inode: dict[int, tuple[str, Owner]] = {}
        for path, info in walk(Path(root)):
            relative = path.relative_to(root).as_posix()
            name = '' if relative == '.' else relative
            owner = self.wanted.get(name)
            if owner is None:
                if not name:
                    raise OwnersError('the volume root has no owner')
                parent = effective[name.rpartition('/')[0]]
                owner = Owner(parent.uid, parent.gid)
            effective[name] = owner
            if info.st_ino in by_inode and by_inode[info.st_ino][1] != owner:
                raise OwnersError(f'{by_inode[info.st_ino][0]} and {name} are one file with two owners')
            by_inode[info.st_ino] = (name, owner)
        return {ino: owner for ino, (_, owner) in by_inode.items()}

    def save(self, path: Path) -> None:
        Path(path).write_text(json.dumps({k: asdict(v) for k, v in sorted(self.wanted.items())}, indent=0) + '\n')

    @classmethod
    def load(cls, path: Path) -> 'Journal':
        journal = cls()
        journal.wanted = {k: Owner(**v) for k, v in json.loads(Path(path).read_text()).items()}
        return journal


def apply(fd: int, base: int, volume_uuid: object, by_inode: dict[int, Owner]) -> int:
    """Rewrite the inodes of the detached volume on `fd` to `by_inode`; return how many.

    Every inode outside APFS's reserved range must be named: one the walk could not reach (a file without a path)
    would keep the builder's own uid, which on the device is mobile's. The one exception is what fseventsd writes in
    the volume's .fseventsd when it is unmounted, after the walk: it is given root's, as fseventsd's own files are.
    """
    volume = apfs_tree.Volume(fd, base, volume_uuid)
    current = volume.inodes()
    unnamed = [i for i in current if i >= FIRST_USER_INODE and i not in by_inode]
    late = [i for i in unnamed if _in_event_log(current, i)]
    if len(late) != len(unnamed):
        first = min(set(unnamed) - set(late))
        raise OwnersError(f'{len(unnamed) - len(late)} inodes have no path to take an owner from ({first} is one)')
    by_inode = dict(by_inode) | {i: Owner(0, 0) for i in late}
    wanted = {}
    for ino, owner in by_inode.items():
        if ino not in current:
            raise OwnersError(f'inode {ino} is not in the volume')
        now = current[ino]
        mode = now.mode if owner.mode is None else (now.mode & ~0o7777) | owner.mode
        flags = now.flags if owner.flags is None else (now.flags & COMPRESSED) | (owner.flags & ~COMPRESSED)
        wanted[ino] = apfs_tree.Inode(owner.uid, owner.gid, mode, flags)
    return volume.rewrite(wanted)


def _in_event_log(inodes: dict[int, apfs_tree.Inode], ino: int) -> bool:
    """Whether `ino` is below the volume's top-level .fseventsd, which fseventsd writes after the walk, as root."""
    for _ in range(len(inodes)):
        inode = inodes.get(ino)
        if inode is None:
            return False
        if inode.parent == ROOT_INODE:
            return inode.name == '.fseventsd'
        ino = inode.parent
    return False


def check(root: Path, by_inode: dict[int, Owner]) -> None:
    """Compare every path below `root`, mounted again with owners, with what was applied (fseventsd's: root's)."""
    wrong = []
    events = Path(root) / '.fseventsd'
    for path, info in walk(Path(root)):
        if path == events or events in path.parents:
            if info.st_uid != 0:
                wrong.append(f"{path}: fseventsd's, but not root's")
            continue
        owner = by_inode.get(info.st_ino)
        if owner is None:
            wrong.append(f'{path}: not in the record')
            continue
        have = (info.st_uid, info.st_gid)
        if have != (owner.uid, owner.gid) or (owner.mode is not None and info.st_mode & 0o7777 != owner.mode):
            wrong.append(f'{path}: {have} {info.st_mode & 0o7777:o}, wanted {owner}')
        elif owner.flags is not None and info.st_flags & ~COMPRESSED != owner.flags:
            wrong.append(f'{path}: flags {info.st_flags:#x}, wanted {owner.flags:#x}')
    if wrong:
        raise OwnersError(f'{len(wrong)} paths are not as recorded, e.g. {wrong[0]}')


def walk(root: Path) -> list[tuple[Path, os.stat_result]]:
    """(path, lstat) of `root` and everything below it, links not followed."""
    found = [(root, os.lstat(root))]
    for directory, dirs, files in os.walk(root):
        for name in sorted(dirs) + sorted(files):
            path = Path(directory) / name
            found.append((path, os.lstat(path)))
    return found


class Immediate:
    """Owners given at once, by root on a volume mounted with owners (a card written in the Mac): a Journal's calls."""

    def set(self, path: str | Path, uid: int, gid: int, mode: int | None = None, flags: int | None = None) -> None:
        os.lchown(path, uid, gid)
        if mode is not None and not os.path.islink(path):
            os.chmod(path, mode)
        if flags is not None:
            os.lchflags(path, (os.lstat(path).st_flags & COMPRESSED) | flags)

    def copy(self, source: Path, under: str | Path = '') -> int:
        """Nothing to do: a copy made by root keeps its source's owners."""
        del source, under
        return 0


Owners = Journal | Immediate
