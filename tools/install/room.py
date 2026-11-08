"""Room in a raw image of one APFS volume (a cryptex being made): measured on the volume, grown on demand, cut to fit.

Nothing here guesses a size. An image starts at what its files will take; make_room() grows it by the shortfall the
mounted volume itself reports, beside the space APFS keeps back from files (reserve()); with_room() grows again and
resumes if a write runs out anyway; fit() cuts the container down to what it holds once nothing more is written,
when macOS still takes the result.
"""

import errno
import os
import shutil
from collections.abc import Callable
from pathlib import Path

from tools.common import files
from tools.install import disks, host, progress

MIB = 1 << 20
BLOCK = 4096
# Growing a container adds a little metadata of its own (its space manager), so the next measure can fall a few blocks
# short again; a volume still short after this many rounds is a bug, not a size.
GROW_ROUNDS = 4


def mib(size: int) -> int:
    return -(-size // MIB) * MIB


def used_bytes(root: Path) -> int:
    """Return what the files on the volume mounted at `root` take, by its own count of used blocks."""
    stats = os.statvfs(root)
    return (stats.f_blocks - stats.f_bfree) * stats.f_frsize


def reserve(root: Path) -> int:
    """Return the free space APFS keeps from files on the volume mounted at `root`.

    Measured by writing one file until ENOSPC on fresh 4, 16, 64 and 256 MiB containers: the last 228, 260, 440 and
    1417 blocks of the free count went unused. A 32nd of the container plus 1 MiB covers each; fit() gives back what
    goes unused.
    """
    stats = os.statvfs(root)
    return stats.f_blocks * stats.f_frsize // 32 + MIB


def blank(image: Path, size: int, name: str, log: host.Log) -> None:
    """Make `image` a raw image of `size` bytes holding one empty case-sensitive APFS volume, as a cryptex is."""
    with open(image, 'wb') as f:
        f.truncate(mib(size))
    with disks.attached(image, log, raw=True) as attachment:
        host.run('newfs_apfs', '-e', '-v', name, '/dev/' + attachment.whole, log=log)


def _grow(image: Path, by: int, log: host.Log) -> None:
    """Make `image` `by` bytes (whole MiB) larger and its APFS container fill it."""
    with open(image, 'r+b') as f:
        f.truncate(image.stat().st_size + mib(by))
    with disks.attached(image, log, raw=True) as attachment:
        host.watch('diskutil', 'apfs', 'resizeContainer', attachment.whole, '0', parse=progress.phases, log=log)


def make_room(image: Path, need: Callable[[Path], int], log: host.Log, what: str) -> None:
    """Grow `image` until its volume, mounted, has need(mount point) bytes free beyond APFS's reserve."""
    for _ in range(GROW_ROUNDS):
        with disks.attached(image, log, raw=True) as image_, disks.mounted(image_.volumes[0], log, False) as root:
            short = need(root) + reserve(root) - shutil.disk_usage(root).free
        if short <= 0:
            return
        log(f'{what}: growing the volume by {mib(short) >> 20} MiB')
        with progress.step(f'{what}: growing the volume', unit=None):
            _grow(image, short, log)
    raise host.HostError(f'{what}: the volume is still short of room after {GROW_ROUNDS} rounds of growing')


def with_room[T](image: Path, need: Callable[[Path], int], work: Callable[[Path], T], log: host.Log, what: str) -> T:
    """Run work(mount point) on `image`'s volume once it has need() bytes free; on ENOSPC grow and run it again.

    `work` must be safe to run again over what an interrupted run left.
    """
    for _ in range(GROW_ROUNDS):
        make_room(image, need, log, what)
        try:
            with disks.attached(image, log, raw=True) as target, disks.mounted(target.volumes[0], log, False) as root:
                return work(root)
        except (OSError, host.HostError) as error:
            if getattr(error, 'errno', None) != errno.ENOSPC and 'No space left on device' not in str(error):
                raise
        log(f'{what}: the volume ran out of room; growing it and starting the step again')
        with disks.attached(image, log, raw=True) as target, disks.mounted(target.volumes[0], log, False) as root:
            more = need(root) + reserve(root)
        _grow(image, more, log)
    raise host.HostError(f'{what}: still out of room after {GROW_ROUNDS} rounds of growing')


def fit(image: Path, apfs_tool: str, check: Callable[[Path], None], log: host.Log, what: str) -> int:
    """Cut the detached container in `image` to the smallest it fits (apfs_tool), and the file with it; return bytes.

    The fit is made on a clone and kept only if check(clone) passes: fsck_apfs passes containers the kernel refuses
    (apfs_tool once kept free queue limits that no longer matched the size), and a cryptex macOS cannot attach would
    not attach on the device either. Then `image` stays as it was.
    """
    blocks, minimum, _, _ = map(int, host.run(apfs_tool, str(image), '0', log=log).split())
    if minimum >= blocks:
        return blocks * BLOCK
    clone = files.partial(image)
    clone.unlink(missing_ok=True)
    host.run('cp', '-c', image, clone, log=log)
    try:
        host.run(apfs_tool, str(clone), '0', str(minimum), log=log)
        os.truncate(clone, minimum * BLOCK)
        check(clone)
    except host.HostError as error:
        log(f'{what}: kept at {blocks * BLOCK >> 20} MiB; fitted to {minimum * BLOCK >> 20} MiB it fails: {error}')
        clone.unlink(missing_ok=True)
        return blocks * BLOCK
    os.replace(clone, image)
    return minimum * BLOCK
