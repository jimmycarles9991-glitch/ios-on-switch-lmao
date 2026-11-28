"""iOS 27: Data's Switch/ as the kernel needs it to take a deploy's updates: root:wheel, no group or other write.

kernel/switch/vfs/pretranslated_updates.c uses Switch/pretranslated-updates only under a Switch/ that nobody but root
can change, and otherwise gives each updated file no redirect (it then runs translated at load time). The install
leaves Switch/ so; a Data volume rebuilt without it, or a Switch/ a Mac made, has another owner. The deploy reads the
owner in its check, with Data mounted read-only with owners (as tools/install/owners_apfs_test.py mounts its images,
without root), and refuses unless that mount shows the tree as 0:0, which proves owners are honoured there. When the
owner is wrong the deploy asks for sudo before it writes anything, then sets it first of all, before any other Data
write, and reads it again: a run that cannot have sudo, or a mount that will not take the owner, stops with nothing
else written.
"""

import os
import stat
from dataclasses import dataclass
from pathlib import Path

from tools.install import card as cards, host, renamed

SUDO = '/usr/bin/sudo'
TREE = 'pretranslated'
CHOWN = '/usr/sbin/chown'
CHMOD = '/bin/chmod'


@dataclass(frozen=True)
class Owner:
    uid: int
    gid: int
    mode: int

    @property
    def kernel_takes_updates(self) -> bool:
        """Whether the kernel takes updates under it."""
        return self.uid == 0 and self.gid == 0 and not self.mode & (stat.S_IWGRP | stat.S_IWOTH)

    def __str__(self) -> str:
        return f'{self.uid}:{self.gid} {self.mode & 0o7777:04o}'


def _owner(path: Path) -> Owner | None:
    try:
        st = os.lstat(path)
    except FileNotFoundError:
        return None
    if not stat.S_ISDIR(st.st_mode):
        raise cards.RefusedError(f"the card's Data has {path.name}, which is not a folder")
    return Owner(st.st_uid, st.st_gid, st.st_mode)


def read(data: str, log: host.Log) -> Owner | None:
    """Switch/'s owner on the card (None: no Switch/ yet), from Data mounted read-only with owners.

    Refused unless that mount shows the tree as root's: the install leaves it 0:0 and the kernel captures it only so,
    so any other answer means owners are not honoured there, and setting Switch/'s would not take.
    """
    with cards.mounted(data, log, read_only=True, owners=True) as data_root:
        folder = renamed.data_folder(data_root)
        owner = _owner(folder)
        tree = _owner(folder / TREE)
        if not tree or (tree.uid, tree.gid) != (0, 0):
            raise cards.RefusedError(
                f'Data mounted with owners shows {folder.name}/{TREE} as {tree or "missing"}, not 0:0: owners are not '
                "honoured on this mount, so Switch/'s owner cannot be set"
            )
        return owner


def planned(owner: Owner | None) -> str:
    if owner and owner.kernel_takes_updates:
        return f'Data Switch/: {owner}, as the kernel requires'
    found = str(owner) if owner else 'made by this deploy'
    return f'Data Switch/: {found}; set to root:wheel without group or other write (sudo)'


def authorize(owner: Owner | None, log: host.Log) -> None:
    """Before the first write: sudo's credentials, when Switch/ will need them."""
    if owner and owner.kernel_takes_updates:
        return
    log('Data Switch/ will need sudo (root:wheel without group or other write): asking now, before writing')
    host.run(SUDO, '-v', log=log)


def ensure(data: str, log: host.Log) -> None:
    """Make Switch/ root:wheel without group or other write, or fail the deploy."""
    with cards.mounted(data, log, owners=True) as data_root:
        folder = Path(data_root) / renamed.DATA_FOLDER
        owner = _owner(folder)
        if owner and owner.kernel_takes_updates:
            return
        if owner is None:
            raise host.HostError(f"the card's Data has no {renamed.DATA_FOLDER}/ after the deploy wrote it")
        log(f'Data Switch/ is {owner}: setting root:wheel without group or other write (sudo)')
        host.run(SUDO, '--', CHOWN, '0:0', folder, log=log)
        host.run(SUDO, '--', CHMOD, 'go-w', folder, log=log)
        owner = _owner(folder)
        if not (owner and owner.kernel_takes_updates):
            raise host.HostError(
                f"Data Switch/ is still {owner}: the kernel will give the deploy's updated files no redirect"
            )
        log(f'Data Switch/: {owner}')
