"""Populate: the composed container's Data and Preboot volumes filled as the user, their owners given offline.

The System volume is mounted twice, one mount after the other: first showing its owners, to record who owns
/private/var's files and the System files iOS 27's translation replaces, then without them, so the user can read every
file it copies. Data and Preboot are written without owners; their journals (owners.py) are resolved while they are
mounted and applied to the detached image, which fsck_apfs checks and a mount that honours owners reads back.
"""

import os
import uuid
from dataclasses import dataclass
from pathlib import Path

from tools.install import card as cards, compose, delta, disks, host, owners, populate, pretranslate, space

VAR = '/private/var'


@dataclass(frozen=True)
class Inputs:
    spec: dict
    deltas: pretranslate.Deltas | None
    images: dict[str, tuple[Path, str]]
    writes: int
    boot_package: str
    gpu_stager: str
    gpu_base: str
    shader_seed: str | None


def _volumes(attachment: disks.Attachment, spec: dict) -> dict[str, str]:
    by_uuid = {(host.disk_info(v) or {}).get('VolumeUUID', '').upper(): v for v in attachment.volumes}
    found = {key: by_uuid.get(volume['uuid'].upper()) for key, volume in spec['volumes'].items()}
    if not all(found.values()):
        raise host.HostError(f"the composed container's volumes did not attach: {found}")
    return found


def _os_entries(deltas: pretranslate.Deltas | None) -> list[dict]:
    return deltas.entries('os') if deltas and 'os' in deltas else []


def _staged(e: dict) -> bool:
    return e['path'].startswith(pretranslate.STAGED_APPS)


def data_entries(entries: list[dict]) -> list[dict]:
    """Return the os delta's entries Data's copy of /private/var takes, paths relative to it."""
    return delta.entries_under([e for e in entries if not _staged(e)], VAR)


def tree_entries(entries: list[dict]) -> list[dict]:
    """Return the os delta's entries the pretranslated tree takes: the System files and the staged system apps."""
    return [e for e in entries if not e['path'].startswith(VAR + '/') or _staged(e)]


def _write(image: Path, volumes: dict[str, str], inputs: Inputs, log: host.Log) -> dict[str, dict[int, owners.Owner]]:
    """Fill Data and Preboot; return each one's owners by inode."""
    data_owners, preboot_owners = owners.Journal(), owners.Journal()
    entries = _os_entries(inputs.deltas)
    with disks.mounted(volumes['system'], log, owners=True, read_only=True) as system:
        data_owners.copy(system / VAR.lstrip('/'))
        originals = {e['path']: os.lstat(system / e['path'].lstrip('/')) for e in entries}
        var_bytes = space.allocated(system / VAR.lstrip('/'))
    space.check(image.parent, var_bytes + inputs.writes, 'populate')
    with (
        disks.mounted(volumes['system'], log, owners=False, read_only=True) as system,
        disks.mounted(volumes['data'], log, owners=False) as data,
        disks.mounted(volumes['preboot'], log, owners=False) as preboot,
    ):
        data_owners.root, preboot_owners.root = data, preboot
        populate.seed_data(system, data, data_owners, log, reseed=True)
        if entries:
            delta.apply(data, data_entries(entries), log, 'Data')
            inputs.deltas.overlay_system(data, originals.__getitem__, tree_entries(entries), data_owners)
        populate.stage_cryptexes(preboot, inputs.spec, inputs.images, preboot_owners, log)
        cards.sync_overlay(data, inputs.boot_package, log)
        populate.stage_gpu(data, inputs.gpu_stager, inputs.gpu_base, log)
        if inputs.shader_seed:
            populate.merge_tree(inputs.shader_seed, data, data_owners, log, 'shader seed')
        disks.settle(data)
        disks.settle(preboot)
        return {'data': data_owners.resolve(data), 'preboot': preboot_owners.resolve(preboot)}


def fill(image: Path, offset: int, inputs: Inputs, log: host.Log) -> None:
    """Populate the container at `offset` in `image` as the user, owners included, and check the result."""
    with disks.attached(image, log, raw=True) as attachment:
        by_inode = _write(image, _volumes(attachment, inputs.spec), inputs, log)
    fd = os.open(image, os.O_RDWR)
    try:
        for key, wanted in by_inode.items():
            volume = uuid.UUID(inputs.spec['volumes'][key]['uuid'])
            log(f'{key}: {owners.apply(fd, offset, volume, wanted)} inodes given their owners')
    finally:
        os.close(fd)
    with disks.attached(image, log, raw=True) as attachment:
        volumes = _volumes(attachment, inputs.spec)
        compose.fsck(attachment.whole + 's1', 'image', log)
        for key, wanted in by_inode.items():
            with disks.mounted(volumes[key], log, owners=True, read_only=True) as root:
                owners.check(root, wanted)
