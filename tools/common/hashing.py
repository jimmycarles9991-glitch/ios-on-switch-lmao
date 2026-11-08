"""SHA-256 of files: one reader for every tool, and many files hashed on every core."""

import concurrent.futures
import hashlib
import os
from collections.abc import Callable, Sequence

# Hashing runs at about 2.5 GB/s on one core and any read size from 1 MiB up keeps up with it; one reused buffer
# spares an allocation per read.
CHUNK = 8 << 20


def sha256_file(path: str | os.PathLike, advance: Callable[[int], None] | None = None) -> str:
    """Return the file's sha256 in hex; `advance` (progress.advance) is told the bytes as they are read."""
    digest = hashlib.sha256()
    buffer = bytearray(CHUNK)
    view = memoryview(buffer)
    with open(path, 'rb', buffering=0) as f:
        while count := f.readinto(buffer):
            digest.update(view[:count])
            if advance:
                advance(count)
    return digest.hexdigest()


def sha256_files(paths: Sequence[str | os.PathLike]) -> list[str]:
    """Return each file's sha256 in order, hashed on every core (hashlib lets go of the GIL while it hashes)."""
    if len(paths) < 2:
        return [sha256_file(p) for p in paths]
    with concurrent.futures.ThreadPoolExecutor(min(len(paths), os.cpu_count() or 4)) as pool:
        return list(pool.map(sha256_file, paths))
