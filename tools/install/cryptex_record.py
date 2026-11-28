"""Preboot's record of a staged cryptex image: <image>.sha256, the sha256 written, bound to the file it was written to.

Whoever writes an image whole (the install's populate, //deploy:sd, //deploy:system) writes the record after it: the
sha256 it wrote and the file's id, size and modification time. The record stands for the file only while all three
still match, so an image another tool, an older checkout or a reinstall replaced (a rename gives it a new id) has no
record. Without one the image is written again; nothing reads the card's gigabytes back to find out what it holds.
"""

import json
from pathlib import Path

from tools.common import files

SUFFIX = '.sha256'


def path(image: str | Path) -> Path:
    image = Path(image)
    return image.with_name(image.name + SUFFIX)


def _identity(image: Path) -> dict:
    st = image.stat()
    return {'inode': st.st_ino, 'size': st.st_size, 'mtime_ns': st.st_mtime_ns}


def write(image: str | Path, sha256: str) -> None:
    """Record that `image`, as it is now, holds the bytes whose sha256 is `sha256`."""
    record = {'sha256': sha256, **_identity(Path(image))}
    files.write_text(path(image), json.dumps(record, sort_keys=True) + '\n', sync=True)


def read(image: str | Path) -> str | None:
    """Return the sha256 the record holds for `image` as it is now, or None (no record, or one of another file)."""
    try:
        record = json.loads(path(image).read_text())
        identity = _identity(Path(image))
    except (OSError, ValueError):
        return None
    if not isinstance(record, dict) or any(record.get(key) != value for key, value in identity.items()):
        return None
    sha256 = record.get('sha256')
    return sha256 if isinstance(sha256, str) else None
