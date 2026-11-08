"""The iOS APFS container, composed in a scratch disk image on the Mac (never on the card).

The IPSW's OS container (System volume 04769D26...) is copied into a raw image, grown to the target partition's size,
given the Data and Preboot volumes, and those get the UUIDs the kernel was built with (diskutil cannot choose UUIDs;
apfs.py rewrites them offline). fsck_apfs checks the result.
"""

import contextlib
import fcntl
import os
import struct
import subprocess
import uuid
from pathlib import Path

from tools.install import apfs, disks, gpt, host, progress, space

COPY_CHUNK = 8 << 20
F_PUNCHHOLE = 99  # <sys/fcntl.h>: deallocate a range of a file (APFS)
START_SECTOR = 2048


def punch(fd: int, offset: int, length: int) -> None:
    """Give a copied range of a scratch image back to the disk, so the scratch and its copy never both exist whole."""
    with contextlib.suppress(OSError):
        fcntl.fcntl(fd, F_PUNCHHOLE, struct.pack('<IIqq', 0, 0, offset, length))


def ours(spec: dict, found: list[tuple[uuid.UUID, int, str]] | None) -> bool:
    have = {(str(u).upper(), role) for u, role, _ in (found or [])}
    return all((v['uuid'], v['role']) in have for v in spec['volumes'].values())


def attach_raw(image: Path, log: host.Log, mount: bool = False) -> str:
    args = ['attach', '-plist'] + ([] if mount else ['-nomount'])
    out = host.plist('hdiutil', *args, '-imagekey', 'diskimage-class=CRawDiskImage', str(image), log=log)
    disk = host.whole_disk_of(out['system-entities'])
    if not disk:
        raise host.HostError(f'hdiutil attach gave no disk for {image}')
    return disk


def fsck(store: str, what: str, log: host.Log) -> None:
    with progress.step(f'fsck_apfs {store} ({what})', unit=None):
        result = host.stream([host.TOOLS['fsck_apfs'], '-n', '/dev/r' + store], progress.phases)
    log.quiet(result.stdout + result.stderr)
    # The verdict is stdout's last line. Warnings go to stderr, among them a fitted container's block count being
    # smaller than its device's, which is what a fit proof's clone of the full-size image always shows.
    tail = result.stdout.strip().splitlines()[-1:] or result.stderr.strip().splitlines()[-1:] or ['?']
    if result.returncode or 'appears to be OK' not in tail[0]:
        raise host.HostError(f'fsck_apfs -n {store} ({what}): {tail[0]}')
    log(f'fsck_apfs {store} ({what}): {tail[0]}')


APFS_CONTAINER = 'EF57347C-0000-11AA-AA11-00306543ECAC'  # hdiutil's hint for the container macOS synthesizes


def _attach_os(os_dmg: str, log: host.Log) -> tuple[str, str]:
    out = host.plist(
        'hdiutil', 'attach', '-plist', '-readonly', '-nomount', '-noverify', '-noautofsck', os_dmg, log=log
    )
    # The APFS physical store, never a volume: a GPT image's Apple_APFS partition (21H461), or the image itself when
    # it is a bare container (24B119's OS dmg). How hdiutil hints the image itself differs between macOS releases
    # (none at all on macOS 27), so the store is the one diskutil names for the synthesized container.
    containers = [e['dev-entry'] for e in out['system-entities'] if e.get('content-hint', '').upper() == APFS_CONTAINER]
    info = host.disk_info(containers[0]) if len(containers) == 1 else None
    stores = (info or {}).get('APFSPhysicalStores', [])
    if len(stores) != 1:
        for d in (e.get('dev-entry', '') for e in out['system-entities']):
            if d.startswith('/dev/disk') and 's' not in d[len('/dev/disk') :]:
                subprocess.run([host.TOOLS['hdiutil'], 'detach', d], capture_output=True, check=False)
        raise host.HostError(f'{os_dmg}: expected one APFS container on one physical store, got {out}')
    store = '/dev/' + stores[0]['APFSPhysicalStore']
    slice_at = store.rfind('s', len('/dev/disk'))
    return (store[:slice_at] if slice_at > 0 else store), store


def os_measure(os_dmg: str, system_uuid: str, log: host.Log) -> tuple[int, int]:
    """Return what the OS image's container uses and what its System volume's /private/var takes, by their own counts.

    Composing writes the first; populating copies the second onto Data.
    """
    whole, source = _attach_os(os_dmg, log)
    try:
        container = host.container_on(source[len('/dev/') :])
        if not container:
            raise host.HostError(f'{os_dmg}: no APFS container on {source}')
        used = int(container['CapacityCeiling']) - int(container['CapacityFree'])
        volume = next(
            (v['DeviceIdentifier'] for v in container['Volumes'] if v['APFSVolumeUUID'].upper() == system_uuid.upper()),
            None,
        )
        if volume is None:
            raise host.HostError(f'{os_dmg}: no System volume {system_uuid}')
        with disks.mounted(volume, log, owners=False, read_only=True) as root:
            return used, space.allocated(root / 'private/var')
    finally:
        host.run('hdiutil', 'detach', whole, log=log)


def _copy_os(os_dmg: str, image: Path, partition_sectors: int, spec: dict, log: host.Log) -> int:
    disk_sectors = START_SECTOR + partition_sectors + 2048
    whole, source = _attach_os(os_dmg, log)
    try:
        os_bytes = int(host.disk_info(source)['TotalSize'])
        raw_source = '/dev/r' + source[len('/dev/') :]
        fd = os.open(raw_source, os.O_RDONLY)
        try:
            found = apfs.Container(fd).volumes()
        finally:
            os.close(fd)
        if (spec['volumes']['system']['uuid'], 1) not in {(str(u).upper(), r) for u, r, _ in found}:
            raise host.HostError(
                'the OS image has no System volume {} ({})'.format(spec['volumes']['system']['uuid'], found)
            )
        with open(image, 'wb') as out:
            out.truncate(disk_sectors * gpt.SECTOR)
        ifd = os.open(image, os.O_RDWR)
        sfd = os.open(raw_source, os.O_RDONLY)
        try:
            first = (gpt.APFS, uuid.uuid4(), START_SECTOR, START_SECTOR + os_bytes // gpt.SECTOR - 1, 'compose')
            for lba, data in gpt.build(disk_sectors, [first]):
                os.pwrite(ifd, data, lba * gpt.SECTOR)
            with progress.step('compose: copying the OS container', os_bytes):
                host.copy_parallel(sfd, 0, ifd, START_SECTOR * gpt.SECTOR, os_bytes)
                os.fsync(ifd)
        finally:
            os.close(sfd)
            os.close(ifd)
        log(f'compose: OS container copied ({os_bytes} bytes)')
        return os_bytes
    finally:
        host.run('hdiutil', 'detach', whole, log=log)


def _grow(image: Path, partition_bytes: int, spec: dict, log: host.Log) -> None:
    disk = attach_raw(image, log)
    try:
        store = disk + 's1'
        with progress.step(f'compose: growing the container to {progress.size(partition_bytes)}', unit=None):
            host.watch(
                'diskutil', 'apfs', 'resizeContainer', store, f'{partition_bytes}B', parse=progress.phases, log=log
            )
        container = host.container_on(store)
        if not container:
            raise host.HostError('the scratch container did not attach')
        for key, role in (('data', 'D'), ('preboot', 'B')):
            host.run(
                'diskutil',
                'apfs',
                'addVolume',
                container['ContainerReference'],
                'Case-sensitive APFS',
                spec['volumes'][key]['name'],
                '-role',
                role,
                '-nomount',
                log=log,
            )
        log(f'compose: grown to {partition_bytes} bytes, Data and Preboot added')
    finally:
        host.run('hdiutil', 'detach', disk, log=log)


def _set_uuids(image: Path, partition_bytes: int, spec: dict, log: host.Log) -> int:
    fd = os.open(image, os.O_RDWR)
    try:
        part = gpt.read(fd, os.fstat(fd).st_size).used[0]
        if part.bytes != partition_bytes:
            raise host.HostError(
                f'resizeContainer left the scratch partition at {part.bytes} bytes, not {partition_bytes}'
            )
        container = apfs.Container(fd, part.first * gpt.SECTOR)
        found = container.volumes()
        mapping = {}
        for key in ('data', 'preboot'):
            want = spec['volumes'][key]
            match = [u for u, role, name in found if role == want['role'] and name == want['name']]
            if len(match) != 1:
                raise host.HostError(f'scratch container: no single {key} volume ({found})')
            mapping[match[0]] = uuid.UUID(want['uuid'])
        patched = container.set_volume_uuids(mapping)
        log(
            'compose: volume UUIDs set offline in {} superblock copies: {}'.format(
                len(patched), ', '.join(f'{o}->{n}' for _, o, n in patched)
            )
        )
        if not ours(spec, container.volumes()):
            raise host.HostError(f'UUID rewrite did not take: {container.volumes()}')
        nx_blocks = int.from_bytes(container.superblocks()[0][40:48], 'little')
        if nx_blocks * container.block_size != partition_bytes:
            raise host.HostError(f'container is {nx_blocks * container.block_size} bytes, partition {partition_bytes}')
        return part.first * gpt.SECTOR
    finally:
        os.close(fd)


def compose(image: Path, os_dmg: str, partition_bytes: int, spec: dict, log: host.Log) -> tuple[int, int]:
    """Write the container into `image` (a new raw disk image whose one partition is exactly `partition_bytes`).

    Returns (the partition's offset in the image, the OS container's size).
    """
    if image.exists():
        image.unlink()
    os_bytes = _copy_os(os_dmg, image, partition_bytes // gpt.SECTOR, spec, log)
    _grow(image, partition_bytes, spec, log)
    offset = _set_uuids(image, partition_bytes, spec, log)
    disk = attach_raw(image, log)
    try:
        fsck(disk + 's1', 'scratch', log)
    finally:
        host.run('hdiutil', 'detach', disk, log=log)
    return offset, os_bytes
