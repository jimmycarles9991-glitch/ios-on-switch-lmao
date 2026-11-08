"""What each stage of //:image writes to the Mac's disk, measured from its inputs and checked before it starts.

    compose   the OS container's used blocks (its copy skips what is zero) and its new volumes' metadata
    populate  what goes onto Data and Preboot: /private/var, the os delta, the cryptex images, the boot package's
              overlay, the GPU bundle and the shader seed; the clone of composed.img itself costs nothing
    pack      the SII parts hold every used block of the container; the populated clone is punched out behind them,
              which gives back what populate wrote, but composed.img keeps the OS container's blocks

So a whole run needs about the OS container twice plus what populate writes. Every number is a byte count of files
or a container's own count of used blocks; KEEP_FREE is the only fixed one.
"""

import json
import os
from pathlib import Path

from tools.install import disks, host, progress

# The Mac's own working room: swap files, logs and the Bazel server write to the same disk while an image is built,
# and a run that took the last byte would stall them. It is policy, not an estimate of anything //:image writes.
KEEP_FREE = 2 << 30


def allocated(path: str | Path | None) -> int:
    """Return the bytes the file, or the files below the directory, take on disk (compressed ones as stored)."""
    if not path or not Path(path).exists():
        return 0
    path = Path(path)
    if not path.is_dir():
        return path.stat().st_blocks * 512
    return sum(
        os.lstat(os.path.join(directory, name)).st_blocks * 512
        for directory, _, names in os.walk(path)
        for name in names
    )


def container_used(store: str) -> int:
    """Return what the APFS container on physical store `store` (e.g. disk5s1) uses, by its own count."""
    container = host.container_on(store)
    if not container:
        raise host.HostError(f'no APFS container on {store}')
    return int(container['CapacityCeiling']) - int(container['CapacityFree'])


def delta_bytes(delta_dir: str | Path | None) -> int:
    """Return the bytes of a delta's translated files (manifest.json beside them)."""
    if not delta_dir:
        return 0
    total = 0
    for manifest in Path(delta_dir).glob('*.manifest.json'):
        for entry in json.loads(manifest.read_text())['files']:
            total += allocated(Path(delta_dir) / entry['file'])
    return total


def populate_bytes(args: object, cryptex_images: list[str | Path]) -> int:
    """Return what populate writes beyond /private/var (measured when System is mounted)."""
    seed = getattr(args, 'shader_seed_size', None)
    seed_bytes = 4096 * json.loads(Path(seed).read_text())['blocks4k'] if seed else allocated(args.shader_seed)
    parts = [
        sum(allocated(image) for image in cryptex_images),
        delta_bytes(getattr(args, 'delta_os', None)),
        allocated(Path(args.boot_package) / 'overlay'),
        allocated(args.gpu_base),
        seed_bytes,
    ]
    return sum(parts)


def check(where: str | Path, need: int, what: str) -> None:
    """Refuse to start `what` unless `where`'s disk has `need` bytes free beside KEEP_FREE."""
    free = disks.free_bytes(where)
    if free < need + KEEP_FREE:
        raise host.HostError(
            f'{what} writes {progress.size(need)} to {where}, which has {progress.size(free)} free; '
            f'{progress.size(need + KEEP_FREE - free)} more is needed (the Mac keeps {progress.size(KEEP_FREE)})'
        )


def check_run(where: str | Path, rows: list[tuple[str, int]], cached: int, log: host.Log) -> None:
    """Say what the whole run writes, stage by stage, against what is free; refuse before the first stage if short.

    `cached`: what Bazel's outputs this run reads (iOS 27's translations) already take on the same disk.
    """
    free, total = disks.free_bytes(where), sum(n for _, n in rows)
    stages = ', '.join(f'{name} {progress.size(n)}' for name, n in rows)
    log(f'space: this run writes {progress.size(total)} ({stages}); {progress.size(free)} free on {where}')
    if cached:
        log(f"space: Bazel's outputs it reads already take {progress.size(cached)} (in the disk cache and output base)")
    written = 0
    for name, n in rows:
        written += n
        if written + KEEP_FREE > free:
            short = progress.size(total + KEEP_FREE - free)
            raise host.HostError(
                f'{name} would run out of disk: the run needs {progress.size(total)} and the Mac keeps '
                f'{progress.size(KEEP_FREE)}, {progress.size(free)} is free; clear {short} and run again'
            )
