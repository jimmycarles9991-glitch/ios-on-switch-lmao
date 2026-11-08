"""An image's container onto a partition from the Mac, the way install-ios.bin's stream.c writes it.

Each 8 MiB chunk of the body is checked against the manifest's SHA-256 before any byte of it is written, the first
also against the manifest's container superblock. ZERO extents are written as zeros: a checkpoint area or internal
pool block left from an earlier container would otherwise be read as part of the new one. Chunks are read and hashed
ahead on other threads; one thread writes, in extent order, because an SD card is fastest written sequentially.
"""

import bisect
import collections
import concurrent.futures
import contextlib
import fcntl
import hashlib
import os
import struct
from collections.abc import Callable, Iterator
from pathlib import Path

from tools.install import apfs, manifest

F_NOCACHE = 48
AHEAD = 4
ZEROS = memoryview(bytes(manifest.HASH_CHUNK))
SECTOR = 512


class StreamError(Exception):
    pass


class Stream:
    """NAME.SII.* read as the one stream they split: bytes at a stream offset, from whichever parts hold them."""

    def __init__(self, directory: Path, m: manifest.Manifest) -> None:
        self.starts, self.fds, self.size = [], [], 0
        try:
            for part in m.parts:
                path = Path(directory) / Path(part.path).name
                if not path.is_file() or path.stat().st_size != part.bytes:
                    raise StreamError(f'{path} is missing or not {part.bytes} bytes: run //:image again')
                fd = os.open(path, os.O_RDONLY)
                self.fds.append(fd)
                with contextlib.suppress(OSError):
                    fcntl.fcntl(fd, F_NOCACHE, 1)
                self.starts.append(self.size)
                self.size += part.bytes
        except BaseException:
            self.close()
            raise

    def pread(self, at: int, n: int) -> bytes:
        pieces = []
        while n:
            i = bisect.bisect_right(self.starts, at) - 1
            end = self.starts[i + 1] if i + 1 < len(self.starts) else self.size
            take = min(n, end - at)
            data = os.pread(self.fds[i], take, at - self.starts[i])
            if len(data) != take or take <= 0:
                raise StreamError(f'short read of the image at stream offset {at}')
            pieces.append(data)
            at, n = at + take, n - take
        return b''.join(pieces)

    def close(self) -> None:
        for fd in self.fds:
            os.close(fd)
        self.fds = []


def _hashes(data: bytes) -> list[bytes]:
    chunk = manifest.HASH_CHUNK
    return [hashlib.sha256(data[i : i + chunk]).digest() for i in range(0, len(data), chunk)]


def boot_files(stream: Stream, m: manifest.Manifest) -> list[tuple[str, bytes]]:
    """Return [(name, bytes)] of the image's boot files from its head, each checked against the manifest."""
    found = []
    for boot in m.boot:
        data = stream.pread(boot.offset, boot.bytes)
        if _hashes(data) != boot.hashes:
            raise StreamError(f'boot file {boot.name} does not match its manifest: run //:image again')
        found.append((boot.name, data))
    return found


def matches(data: bytes, boot: manifest.Boot) -> bool:
    return len(data) == boot.bytes and _hashes(data) == boot.hashes


def superblock_ok(m: manifest.Manifest, block: bytes) -> bool:
    """Return whether `block` is the container superblock the manifest describes (switch_image_check_superblock)."""
    if len(block) < m.block_size or block[32:36] != apfs.NX_MAGIC:
        return False
    size, count = struct.unpack_from('<IQ', block, 36)
    return (
        size == m.block_size
        and size * count == m.container_bytes
        and block[72:88] == m.container_uuid.bytes
        and apfs.checksum_ok(block[:size])
    )


def chunks(stream: Stream, m: manifest.Manifest) -> Iterator[bytes]:
    """Yield the body's chunks in order, each only once its hash (and the first's superblock) has been checked."""

    def load(j: int) -> bytes:
        at = j * manifest.HASH_CHUNK
        data = stream.pread(manifest.BODY_OFFSET + at, min(manifest.HASH_CHUNK, m.source_bytes - at))
        if hashlib.sha256(data).digest() != m.hashes[j]:
            raise StreamError(f'chunk {j} of the image does not match its manifest: run //:image again')
        if j == 0 and not superblock_ok(m, data):
            raise StreamError("the image's first block is not the container its manifest describes")
        return data

    count = len(m.hashes)
    with concurrent.futures.ThreadPoolExecutor(AHEAD) as pool:
        queued = collections.deque(pool.submit(load, j) for j in range(min(AHEAD, count)))
        following = len(queued)
        while queued:
            data = queued.popleft().result()
            if following < count:
                queued.append(pool.submit(load, following))
                following += 1
            yield data


class Body:
    """The body's checked chunks as one run of bytes, taken in pieces of at most what is left of a chunk."""

    def __init__(self, source: Iterator[bytes]) -> None:
        self.source, self.view = source, memoryview(b'')

    def take(self, n: int) -> memoryview:
        if not self.view:
            chunk = next(self.source, None)
            if chunk is None:
                raise StreamError("the image's body ends before its extents do")
            self.view = memoryview(chunk)
        piece, self.view = self.view[:n], self.view[n:]
        return piece

    def finished(self) -> bool:
        return not self.view and next(self.source, None) is None


def _write(fd: int, offset: int, data: memoryview, limit: int) -> None:
    if offset < 0 or offset + len(data) > limit or (offset | len(data)) % SECTOR:
        raise StreamError(f'a write of {len(data)} bytes at {offset} is outside the partition ({limit} bytes)')
    if os.pwrite(fd, data, offset) != len(data):
        raise StreamError(f'short write at partition offset {offset}')


def install(m: manifest.Manifest, stream: Stream, fd: int, partition_bytes: int, advance: Callable[[int], None]) -> int:
    """Write the image's extents onto the partition open at `fd` (offset 0 its first byte); return the bytes written.

    `advance` (progress.advance) is told each write's bytes.
    """
    end = max(at + n for at, _, n in m.extents)
    if end > partition_bytes or m.minimum_bytes > partition_bytes:
        raise StreamError(f'the image needs {max(end, m.minimum_bytes)} bytes; the partition has {partition_bytes}')
    body, written = Body(chunks(stream, m)), 0
    for at, source, n in m.extents:
        done = 0
        while done < n:
            left = n - done
            data = ZEROS[: min(left, len(ZEROS))] if source == manifest.ZERO else body.take(left)
            _write(fd, at + done, data, partition_bytes)
            done += len(data)
            written += len(data)
            advance(len(data))
    if not body.finished():
        raise StreamError("the image's body is longer than its extents")
    os.fsync(fd)
    return written
