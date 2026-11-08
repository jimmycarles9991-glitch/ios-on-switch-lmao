"""Which ranges of a composed partition image install-ios.bin must write, and from where.

The image is a sparse file. Its data ranges are copied. APFS reads only what its own structures reference, so the
partition's other bytes (the Linux or Android data it held before) may stay, with one exception: on mount, APFS
scans the checkpoint descriptor area for the newest valid superblock. A stale one from an earlier container there
would win, so the descriptor area is written whole, holes as zeros. The checkpoint data area and the internal pool
are read only where the checkpoint map or the pool's bitmap points, which is data, so their holes stay holes.
"""

import os
import struct
import uuid

from tools.install import manifest

SEEK_HOLE, SEEK_DATA = getattr(os, 'SEEK_HOLE', 3), getattr(os, 'SEEK_DATA', 4)
NX_MAGIC = b'NXSB'
NX_NONCONTIGUOUS = 1 << 31


class ExtentError(Exception):
    pass


def data_ranges(fd: int, offset: int, size: int, block: int) -> list[tuple[int, int]]:
    """[(start, end)] relative to `offset`: the file's data within [offset, offset + size), widened to blocks."""
    out = []
    position = 0
    while position < size:
        try:
            start = os.lseek(fd, offset + position, SEEK_DATA) - offset
        except OSError:
            break
        if start >= size:
            break
        end = min(os.lseek(fd, offset + start, SEEK_HOLE) - offset, size)
        start -= start % block
        end = min(end + -end % block, size)
        if out and start <= out[-1][1]:
            out[-1] = (out[-1][0], max(out[-1][1], end))
        else:
            out.append((start, end))
        position = end
    return out


def container(nxsb: bytes) -> tuple[int, int, uuid.UUID, list[tuple[int, int]]]:
    """(block size, block count, container UUID, [(start, end)] of the checkpoint descriptor area) from block 0."""
    if nxsb[32:36] != NX_MAGIC:
        raise ExtentError('partition offset 0 is not an APFS container superblock')
    block, count = struct.unpack_from('<IQ', nxsb, 36)
    desc_blocks, desc_base = struct.unpack_from('<I', nxsb, 104)[0], struct.unpack_from('<Q', nxsb, 112)[0]
    if desc_blocks & NX_NONCONTIGUOUS:
        raise ExtentError('the checkpoint descriptor area is not contiguous')
    return block, count, uuid.UUID(bytes=nxsb[72:88]), [(desc_base * block, (desc_base + desc_blocks) * block)]


def plan(
    data: list[tuple[int, int]],
    required: list[tuple[int, int]],
    size: int,
    block: int,
    limit: int = manifest.MAX_EXTENTS,
) -> list[tuple[int, int, int]]:
    """Plan the extents [(offset, source offset or ZERO, bytes)] that rebuild the partition.

    Every data range is copied, the parts of `required` ranges that hold no data are written as zeros. Close copy
    extents are joined (the gap copied as zeros) until there are at most `limit` extents.
    """
    merged = _merge(_marks(data, required, size))
    gap = block
    while len(merged) > limit:
        merged = _join(merged, gap)
        gap *= 2
    extents, source = [], 0
    for start, end, copy in merged:
        extents.append((start, source if copy else manifest.ZERO, end - start))
        if copy:
            source += end - start
    if not extents or extents[0][0] != 0 or extents[0][1] == manifest.ZERO:
        raise ExtentError("the image's first block holds no data")
    return extents


def _marks(data: list[tuple[int, int]], required: list[tuple[int, int]], size: int) -> list[tuple[int, int, bool]]:
    """Return the data ranges to copy and the parts of `required` ranges no data covers (zeros), sorted."""
    marks = [(start, end, True) for start, end in data]
    for start, end in required:
        if end > size:
            raise ExtentError(f'range {start:#x}-{end:#x} is beyond the partition')
        at = start
        for d_start, d_end in data:
            if d_end <= at or d_start >= end:
                continue
            if d_start > at:
                marks.append((at, d_start, False))
            at = max(at, d_end)
        if at < end:
            marks.append((at, end, False))
    return sorted(marks)


def _merge(marks: list[tuple[int, int, bool]]) -> list[tuple[int, int, bool]]:
    merged = []
    for start, end, copy in marks:
        if merged and merged[-1][2] == copy and start <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], end), copy)
        else:
            merged.append((start, end, copy))
    return merged


def _join(merged: list[tuple[int, int, bool]], gap: int) -> list[tuple[int, int, bool]]:
    out = []
    for start, end, copy in merged:
        if out and copy and out[-1][2] and start - out[-1][1] <= gap:
            out[-1] = (out[-1][0], end, True)
        else:
            out.append((start, end, copy))
    return out
