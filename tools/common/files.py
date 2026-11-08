"""Files written whole or not at all: each is written beside its place as <name>.partial and renamed over it.

The partial name keeps the file's own name in front: a run that stops leaves NAME.partial, which no reader of NAME
takes for NAME (hekate lists every *.ini in bootloader/ini, so a hidden .partial-ios.ini would be a second entry).
"""

import contextlib
import os
import shutil
from collections.abc import Iterator
from pathlib import Path

SUFFIX = '.partial'


def partial(path: str | os.PathLike) -> Path:
    """Return where `path` is written before it is renamed into place."""
    path = Path(path)
    return path.with_name(path.name + SUFFIX)


def _fsync(path: Path) -> None:
    fd = os.open(path, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


@contextlib.contextmanager
def replacing(path: str | os.PathLike, *, sync: bool = False) -> Iterator[Path]:
    """Yield the partial path to write `path` at; rename it over `path` when the block ends, delete it if it raises.

    `sync` flushes it to the device first (a card pulled out after the rename then holds the whole file).
    """
    part = partial(path)
    part.unlink(missing_ok=True)
    try:
        yield part
        if sync:
            _fsync(part)
        os.replace(part, path)
    except BaseException:
        part.unlink(missing_ok=True)
        raise


def write_bytes(path: str | os.PathLike, data: bytes, *, mode: int | None = None, sync: bool = False) -> None:
    """Make `path` hold `data` (with `mode`, if given) or stay as it was."""
    with replacing(path, sync=sync) as part:
        part.write_bytes(data)
        if mode is not None:
            part.chmod(mode)


def write_text(path: str | os.PathLike, text: str, *, sync: bool = False) -> None:
    write_bytes(path, text.encode(), sync=sync)


def copy(source: str | os.PathLike, target: str | os.PathLike, *, keep_times: bool = False) -> None:
    """Make `target` a copy of `source`'s bytes (and its mode and times with `keep_times`) or leave it as it was."""
    with replacing(target) as part:
        (shutil.copy2 if keep_times else shutil.copyfile)(source, part)
