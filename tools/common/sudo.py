"""Work that needs root: the same command again under sudo, and what root makes in the user's places given back.

A directory root makes below the user's home stays root's, and the user cannot delete it or anything in it without
sudo; a file root makes in a directory the user owns can still be deleted, but not rewritten. So every directory a
run under sudo makes goes back to the user who ran sudo as it is made, and so does every file it leaves for them.
"""

import os
import sys
from collections.abc import Sequence
from pathlib import Path
from typing import NoReturn


def rerun(script: str, argv: Sequence[str]) -> NoReturn:
    """Run `script` again under sudo with `argv`, which names its inputs by absolute path (sudo drops Bazel's env)."""
    command = ['sudo', '--', sys.executable, os.path.abspath(script), *argv]
    print('running under sudo:', ' '.join(command), flush=True)
    os.execvp('sudo', command)


def user() -> tuple[int, int] | None:
    """Return the uid and gid of the user who ran sudo, None unless this is a run as root under sudo."""
    uid, gid = os.environ.get('SUDO_UID'), os.environ.get('SUDO_GID')
    if os.geteuid() != 0 or not uid or not gid:
        return None
    return int(uid), int(gid)


def give_back(path: str | os.PathLike) -> None:
    """Give a path made under sudo to the user who ran sudo; outside sudo, leave it as it is."""
    owner = user()
    if owner:
        os.chown(path, *owner, follow_symlinks=False)


def user_directory(path: str | os.PathLike) -> Path:
    """Make the directory `path` with its missing parents, each the user's who ran sudo (made as root); return it."""
    path = Path(path)
    missing = [p for p in (path, *path.parents) if not p.exists()]
    path.mkdir(parents=True, exist_ok=True)
    for made in missing:
        give_back(made)
    return path
