"""iOS 27: the card's translated System files against this build's os.trustcache, and the updates keeping them current.

The tree under Switch/pretranslated is placed once, by the install (tools/install/pretranslate.py), and the kernel
refuses any change to it. A file this build translates differently (an edit of it: the card's manifest names a CDHash
that the boot package's os.trustcache no longer holds) is carried by the build instead
(//install:pretranslated_updates). The deploy puts it under Switch/pretranslated-updates, and the kernel points the
tree's entry at it when the boot package's /chosen list names it (kernel/switch/vfs/pretranslated.c). A stale file the
build does not carry needs a new tree, which only a reinstall of the container places, and that replaces Data.
"""

import json
import shutil
from pathlib import Path

from tools.common import files, hashing
from tools.install import card as cards, host, pretranslate, renamed
from tools.trustcache import trustcache

MANIFEST = Path(pretranslate.OVERLAY_MANIFEST).name
UPDATES = Path(pretranslate.UPDATES).name
SHOWN = 4
REINSTALL = (
    'reinstall the container, which replaces Data (bazel run //install:sd --config=ios27 -- <diskN> '
    '--reinstall-container)'
)


def untrusted(manifest: dict, module: bytes) -> list[str]:
    """Return the paths of the manifest's trusted files with a cdhash the module does not hold."""
    trusted = {entry[: trustcache.CDHASH_BYTES].hex() for entry in trustcache.module_entries(module)}
    return [
        f['path']
        for f in manifest.get('files', [])
        if f.get('trust') and any(cdhash.lower() not in trusted for cdhash in f.get('cdhash', []))
    ]


def _shown(paths: list[str]) -> str:
    return ', '.join(paths[:SHOWN]) + (f' and {len(paths) - SHOWN} more' if len(paths) > SHOWN else '')


def plan(data_root: Path, trust: Path, carried: Path) -> dict[str, str]:
    """Return the updates the card needs, {System path: CDHash}; refuse stale files the build does not carry.

    `trust` is this build's os.trustcache, `carried` its //install:pretranslated_updates directory.
    """
    manifest = renamed.data_folder(data_root) / MANIFEST
    if not manifest.is_file():
        raise cards.RefusedError(f"the card's Data has no {renamed.DATA_FOLDER}/{MANIFEST}: {REINSTALL}")
    module = Path(trust).read_bytes()
    trusted = {entry[: trustcache.CDHASH_BYTES].hex() for entry in trustcache.module_entries(module)}
    stale = untrusted(json.loads(manifest.read_text()), module)
    offered = json.loads((Path(carried) / pretranslate.UPDATES_LIST).read_text())
    missing = [path for path in stale if offered.get(path) not in trusted]
    if missing:
        raise cards.RefusedError(
            f"{len(missing)} of the card's translated System files are not in this build's os.trustcache and the "
            f'build carries no update for them, so the kernel would refuse them at exec ({_shown(missing)}): '
            f'{REINSTALL}'
        )
    return {path: offered[path] for path in stale}


def _remove(path: Path) -> None:
    if path.is_dir() and not path.is_symlink():
        shutil.rmtree(path)
    else:
        path.unlink()


def stage(data_root: Path, carried: Path, wanted: dict[str, str], log: host.Log) -> None:
    """Make Data's Switch/pretranslated-updates hold exactly the `wanted` updates from `carried`; none: no folder."""
    root = renamed.data_folder(data_root) / UPDATES
    if not wanted:
        if root.exists() or root.is_symlink():
            _remove(root)
            log("pretranslated updates: removed (the card's tree is this build's)")
        return
    if root.is_symlink() or (root.exists() and not root.is_dir()):
        _remove(root)
    keep = {path.lstrip('/') for path in wanted}
    if root.is_dir():
        for path in sorted(root.rglob('*'), reverse=True):
            relative = path.relative_to(root).as_posix()
            if path.is_symlink() or (path.is_file() and relative not in keep):
                path.unlink()
            elif path.is_dir() and not any(path.iterdir()):
                path.rmdir()
    for relative in sorted(keep):
        source, target = Path(carried) / relative, root / relative
        if target.is_file() and hashing.sha256_file(target) == hashing.sha256_file(source):
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        with files.replacing(target, sync=True) as partial:
            shutil.copyfile(source, partial)
            partial.chmod(0o755)
    log(f'pretranslated updates: {", ".join(sorted(wanted))}')
