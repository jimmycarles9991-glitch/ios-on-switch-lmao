"""A composed partition image into NAME.SII.001, .002, ...: the container's body, then the head.

The body is read and hashed in HASH_CHUNK pieces on every core, written in order, and punched out of the image behind,
so the image and its copy never both exist whole. The head is written apart from the body: a change to the boot files
alone rewrites the first BODY_OFFSET bytes of NAME.SII.001 and nothing else.
"""

import concurrent.futures
import hashlib
import os
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path

from tools.install import compose, extents, host, manifest, progress

ALIGN = 4096
WORKERS = os.cpu_count() or 4
IN_FLIGHT = 2 * WORKERS


def part_name(name: str, index: int) -> str:
    return f'{name}.SII.{index + 1:03d}'


def part_paths(directory: Path, name: str, count: int) -> list[Path]:
    return [Path(directory) / part_name(name, i) for i in range(count)]


def _pieces(plan: list[tuple[int, int, int]]) -> Iterator[list[tuple[int, int]]]:
    """Each body chunk as the image ranges it is read from."""
    pending, filled = [], 0
    for at, source, n in plan:
        if source == manifest.ZERO:
            continue
        done = 0
        while done < n:
            take = min(manifest.HASH_CHUNK - filled, n - done)
            pending.append((at + done, take))
            filled += take
            done += take
            if filled == manifest.HASH_CHUNK:
                yield pending
                pending, filled = [], 0
    if pending:
        yield pending


class Writer:
    """The stream's parts, created at their sizes; bytes land at their stream offset from any thread."""

    def __init__(self, directory: Path, name: str, sizes: list[int]) -> None:
        self.starts, self.fds, self.at = [], [], manifest.BODY_OFFSET
        start = 0
        for path, size in zip(part_paths(directory, name, len(sizes)), sizes, strict=True):
            fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o644)
            os.ftruncate(fd, size)
            self.starts.append(start)
            self.fds.append(fd)
            start += size

    def pwrite(self, at: int, data: bytes) -> None:
        while data:
            i = max(j for j, start in enumerate(self.starts) if start <= at)
            end = self.starts[i + 1] if i + 1 < len(self.starts) else at + len(data)
            n = min(len(data), end - at)
            os.pwrite(self.fds[i], data[:n], at - self.starts[i])
            at, data = at + n, data[n:]

    def write(self, data: bytes) -> None:
        self.pwrite(self.at, data)
        self.at += len(data)

    def close(self) -> None:
        for fd in self.fds:
            os.close(fd)
        self.fds = []


def _chunk(fd: int, offset: int, ranges: list[tuple[int, int]], writer: Writer, at: int) -> bytes:
    """Read one body chunk from the image, write it to its place in the stream, return its SHA-256."""
    data = b''.join(os.pread(fd, n, offset + start) for start, n in ranges)
    if len(data) != sum(n for _, n in ranges):
        raise extents.ExtentError(f'short read of the image at {ranges[0][0]:#x}')
    writer.pwrite(at, data)
    return hashlib.sha256(data).digest()


def body(fd: int, offset: int, plan: list[tuple[int, int, int]], writer: Writer, punch: bool) -> list[bytes]:
    """Read, write and hash every body chunk on every core; punch the image behind, in order.

    Each chunk counts toward the running step (progress) once it is written.
    """
    hashes = []
    with concurrent.futures.ThreadPoolExecutor(WORKERS) as pool:
        queued = []
        for j, ranges in enumerate(_pieces(plan)):
            at = manifest.BODY_OFFSET + j * manifest.HASH_CHUNK
            queued.append((ranges, pool.submit(_chunk, fd, offset, ranges, writer, at)))
            while len(queued) > IN_FLIGHT or (queued and queued[0][1].done()):
                hashes.append(_settle(fd, offset, queued.pop(0), punch))
        hashes += [_settle(fd, offset, item, punch) for item in queued]
    return hashes


def _settle(fd: int, offset: int, item: tuple, punch: bool) -> bytes:
    ranges, future = item
    digest = future.result()
    progress.advance(sum(n for _, n in ranges))
    if punch:
        for at, n in ranges:
            compose.punch(fd, offset + at, n)
    return digest


def boot_records(boot_files: list[tuple[str, Path]]) -> list[manifest.Boot]:
    """Place the boot files in the boot region, in order, each on a 4 KiB boundary, with their chunk hashes."""
    records, at = [], manifest.BOOT_OFFSET
    for name, source in boot_files:
        data = Path(source).read_bytes()
        hashes = [
            hashlib.sha256(data[i : i + manifest.HASH_CHUNK]).digest() for i in range(0, len(data), manifest.HASH_CHUNK)
        ]
        records.append(manifest.Boot(name, at, len(data), hashes))
        at += -(-len(data) // ALIGN) * ALIGN
    if at > manifest.BODY_OFFSET:
        raise manifest.ManifestError(f'the boot files take {at - manifest.BOOT_OFFSET} bytes, more than the region')
    return records


def write_head(directory: Path, m: manifest.Manifest, boot_files: list[tuple[str, Path]]) -> None:
    """Write the manifest and the boot files into NAME.SII.001's first BODY_OFFSET bytes."""
    head = bytearray(manifest.BODY_OFFSET)
    raw = manifest.encode(m)
    head[: len(raw)] = raw
    for record, (_, source) in zip(m.boot, boot_files, strict=True):
        head[record.offset : record.offset + record.bytes] = Path(source).read_bytes()
    path = Path(directory) / Path(m.parts[0].path).name
    with open(path, 'r+b' if path.exists() else 'wb') as f:
        f.write(head)


@dataclass
class Source:
    """The composed container: the image file, its offset there, and apfs_tool's measure of it in bytes.

    The measure has container_bytes, minimum_bytes and the internal pool's [pool_start, pool_end).
    """

    image: Path
    offset: int
    layout: dict


@dataclass
class Target:
    """Where NAME.SII.* goes on the Mac, the FAT folder its manifest names, its label and its boot files."""

    directory: Path
    fat_dir: str
    name: str
    label: str
    boot_files: list[tuple[str, Path]]


def pack(source: Source, target: Target, log: host.Log, punch: bool = True) -> manifest.Manifest:
    """Pack the container into the target's NAME.SII.*; return the Manifest."""
    directory, name, layout = Path(target.directory), target.name, source.layout
    for stale in directory.glob(f'{name}.SII.*'):
        stale.unlink()
    fd = os.open(source.image, os.O_RDWR)
    try:
        block, count, container, areas = extents.container(os.pread(fd, 4096, source.offset))
        size, minimum = layout['container_bytes'], layout['minimum_bytes']
        if block * count != size:
            raise extents.ExtentError(f'the container is {block * count} bytes, its measure {size}')
        data = [(a, min(b, minimum)) for a, b in extents.data_ranges(fd, source.offset, size, block) if a < minimum]
        plan = extents.plan(data, areas, minimum, block)
        sizes = manifest.split(manifest.BODY_OFFSET + sum(n for _, s, n in plan if s != manifest.ZERO))
        log(f'pack: {len(plan)} extents, {sum(sizes)} bytes in {len(sizes)} parts, {WORKERS} workers')
        writer = Writer(directory, name, sizes)
        try:
            with progress.step(f'pack: writing {name}.SII', sum(sizes) - manifest.BODY_OFFSET):
                hashes = body(fd, source.offset, plan, writer, punch)
        finally:
            writer.close()
    finally:
        os.close(fd)
    parts = [manifest.Part(f'{target.fat_dir}/{part_name(name, i)}', n) for i, n in enumerate(sizes)]
    boot = boot_records(target.boot_files)
    m = manifest.Manifest(size, minimum, block, container, target.label, parts, plan, hashes, boot)
    write_head(directory, m, target.boot_files)
    log(f'pack: {name}.SII written ({len(parts)} parts, minimum partition {minimum >> 20} MiB)')
    return m
