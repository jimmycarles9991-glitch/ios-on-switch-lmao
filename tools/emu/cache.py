"""One machine-wide, content-addressed cache for the emulator's heavy, shared builds.

Every worktree and output base on this Mac shares ~/Library/Caches/switch-ios/<kind>/<key>/. An entry is built once:
the builder holds an exclusive lock on the kind's directory while it builds into a temporary directory, then renames it
into place, so a reader sees a whole entry or none, and a second builder waits and then reuses the first one's.
"""

import fcntl
import hashlib
import json
import os
import pwd
import shutil
from collections.abc import Callable, Iterator
from contextlib import contextmanager
from pathlib import Path

# The user's own home, not $HOME: Bazel gives a test its own HOME, and the tests read this Mac's builds too.
ROOT = Path(pwd.getpwuid(os.getuid()).pw_dir) / 'Library/Caches/switch-ios'


def digest(parts: dict) -> str:
    """Return the key of a build: a hash of everything that changes its output, as canonical JSON."""
    return hashlib.sha256(json.dumps(parts, sort_keys=True).encode()).hexdigest()[:24]


def file_digest(path: Path) -> str:
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        while chunk := f.read(1 << 20):
            h.update(chunk)
    return h.hexdigest()


@contextmanager
def locked(base: Path) -> Iterator[Path]:
    """Hold the lock of a kind's directory (blocking) and yield it."""
    base.mkdir(parents=True, exist_ok=True)
    with (base / 'lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        try:
            yield base
        finally:
            fcntl.flock(lock, fcntl.LOCK_UN)


def existing(base: Path, key: str) -> Path | None:
    """Return the entry for key in a kind's directory if it has been built, without building it."""
    done = base / 'entries' / key
    return done if done.is_dir() else None


def entry(base: Path, key: str, make: Callable[[Path, Path], None]) -> Path:
    """Return the entry for key in a kind's directory, building it with make(tmp, base) first if it is not there yet."""
    done = base / 'entries' / key
    if done.is_dir():
        return done
    with locked(base):
        if done.is_dir():
            return done
        tmp = base / 'entries' / f'.{key}.partial'
        shutil.rmtree(tmp, ignore_errors=True)
        tmp.mkdir(parents=True)
        make(tmp, base)
        tmp.rename(done)
    return done
