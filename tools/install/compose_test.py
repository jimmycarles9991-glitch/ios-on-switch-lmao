"""compose's attaches: an OS image's APFS physical store, and an attached image's container.

_attach_os finds the store as hdiutil attach and diskutil list it. os_measure and _grow read the container by the
container disk attach names, which answers while diskutil's whole APFS listing still lags behind the attach.
"""

import contextlib
import os
import struct
import tempfile
import unittest
import uuid
from pathlib import Path
from unittest import mock

from tools.install import apfs, compose, gpt

APFS = '7C3457EF-0000-11AA-AA11-00306543ECAC'
APFS_CONTENT = '41504653-0000-11AA-AA11-00306543ECAC'
CONTAINER = 'EF57347C-0000-11AA-AA11-00306543ECAC'

# 21H461's OS dmg: a GPT disk with one APFS partition, and the container macOS synthesizes for it.
GPT_IMAGE = [
    {'dev-entry': '/dev/disk32', 'content-hint': 'GUID_partition_scheme', 'potentially-mountable': False},
    {'dev-entry': '/dev/disk32s1', 'content-hint': APFS, 'potentially-mountable': False},
    {'dev-entry': '/dev/disk33', 'content-hint': CONTAINER, 'unmapped-content-hint': CONTAINER},
    {'dev-entry': '/dev/disk33s1', 'volume-kind': 'apfs', 'potentially-mountable': True},
]
# 24B119's OS dmg: a bare APFS container, whose volume carries the APFS hint as well.
BARE_CONTAINER = [
    {'dev-entry': '/dev/disk32', 'content-hint': APFS_CONTENT, 'potentially-mountable': False},
    {
        'dev-entry': '/dev/disk33s1',
        'content-hint': APFS_CONTENT,
        'unmapped-content-hint': APFS_CONTENT,
        'potentially-mountable': True,
        'volume-kind': 'apfs',
    },
    {'dev-entry': '/dev/disk33', 'content-hint': CONTAINER, 'unmapped-content-hint': CONTAINER},
]
# The same image as macOS 27's hdiutil lists it: no hint on the image itself.
BARE_CONTAINER_UNHINTED = [{'dev-entry': '/dev/disk32', 'potentially-mountable': False}, *BARE_CONTAINER[1:]]


class AttachOS(unittest.TestCase):
    def attach(self, entities: list[dict], store: str) -> tuple[str, str, str]:
        stores = {'/dev/disk33': {'APFSPhysicalStores': [{'APFSPhysicalStore': store}]}}
        with (
            mock.patch.object(compose.host, 'plist', return_value={'system-entities': entities}),
            mock.patch.object(compose.host, 'disk_info', side_effect=stores.get),
        ):
            return compose._attach_os('os.dmg', log=None)

    def test_a_gpt_image_gives_its_apfs_partition_and_the_disk_around_it(self) -> None:
        self.assertEqual(self.attach(GPT_IMAGE, 'disk32s1'), ('/dev/disk32', '/dev/disk32s1', 'disk33'))

    def test_a_bare_container_image_is_its_own_store(self) -> None:
        self.assertEqual(self.attach(BARE_CONTAINER, 'disk32'), ('/dev/disk32', '/dev/disk32', 'disk33'))

    def test_the_store_does_not_depend_on_how_hdiutil_hints_the_image(self) -> None:
        self.assertEqual(self.attach(BARE_CONTAINER_UNHINTED, 'disk32'), ('/dev/disk32', '/dev/disk32', 'disk33'))

    def test_no_synthesized_container_is_refused(self) -> None:
        with (
            mock.patch.object(compose.host, 'plist', return_value={'system-entities': GPT_IMAGE[:2]}),
            mock.patch.object(compose.subprocess, 'run'),
            self.assertRaises(compose.host.HostError),
        ):
            compose._attach_os('os.dmg', log=None)


SCRATCH_IMAGE = [
    {'dev-entry': '/dev/disk40', 'content-hint': 'GUID_partition_scheme', 'potentially-mountable': False},
    {'dev-entry': '/dev/disk40s1', 'content-hint': APFS, 'potentially-mountable': False},
    {'dev-entry': '/dev/disk41', 'content-hint': CONTAINER, 'unmapped-content-hint': CONTAINER},
]
SYSTEM_UUID = '11111111-2222-3333-4444-555555555555'


def container(reference: str, store: str) -> dict:
    return {
        'ContainerReference': reference,
        'CapacityCeiling': 1 << 30,
        'CapacityFree': 1 << 28,
        'PhysicalStores': [{'DeviceIdentifier': store}],
        'Volumes': [{'DeviceIdentifier': reference + 's1', 'APFSVolumeUUID': SYSTEM_UUID}],
    }


class LaggingListing:
    """hdiutil and diskutil as they answer while the whole APFS listing has not yet caught up with an attach.

    The container disk attach names answers by itself; the listing has no container yet (the forced delay).
    """

    def __init__(self, entities: list[dict], containers: dict[str, dict]) -> None:
        self.entities, self.containers = entities, containers

    def plist(self, tool: str, *args: object, **_: object) -> dict:
        if tool == 'hdiutil':
            return {'system-entities': self.entities}
        if args[:3] == ('apfs', 'list', '-plist') and len(args) == 4:
            return {'Containers': [self.containers[str(args[3])]]}
        if args == ('apfs', 'list', '-plist'):
            return {'Containers': []}
        raise AssertionError(f'unexpected {tool} {args}')


class ContainerOfAnAttach(unittest.TestCase):
    def test_measuring_an_os_image_reads_the_container_attach_named(self) -> None:
        tools = LaggingListing(BARE_CONTAINER, {'disk33': container('disk33', 'disk32')})
        stores = {'/dev/disk33': {'APFSPhysicalStores': [{'APFSPhysicalStore': 'disk32'}]}}
        with (
            mock.patch.object(compose.host, 'plist', side_effect=tools.plist),
            mock.patch.object(compose.host, 'disk_info', side_effect=stores.get),
            mock.patch.object(compose.host, 'run'),
            mock.patch.object(compose.disks, 'mounted', return_value=contextlib.nullcontext(Path('/'))),
            mock.patch.object(compose.space, 'allocated', return_value=4096),
        ):
            self.assertEqual(compose.os_measure('os.dmg', SYSTEM_UUID, None), ((1 << 30) - (1 << 28), 4096))

    def test_growing_the_scratch_container_adds_its_volumes_to_the_container_attach_named(self) -> None:
        tools = LaggingListing(SCRATCH_IMAGE, {'disk41': container('disk41', 'disk40s1')})
        spec = {'volumes': {key: {'name': key.title()} for key in ('data', 'preboot')}}
        with (
            mock.patch.object(compose.host, 'plist', side_effect=tools.plist),
            mock.patch.object(compose.host, 'watch'),
            mock.patch.object(compose.host, 'run') as run,
        ):
            compose._grow(Path('scratch.img'), 1 << 30, spec, mock.Mock())
        added = [call.args[3] for call in run.call_args_list if call.args[:3] == ('diskutil', 'apfs', 'addVolume')]
        self.assertEqual(added, ['disk41', 'disk41'])

    def test_set_uuids_also_sets_volume_feature_flags(self) -> None:
        bs = 4096
        part_blocks = 64
        part_bytes = part_blocks * bs
        part_sectors = part_bytes // gpt.SECTOR
        disk_sectors = compose.START_SECTOR + part_sectors + 2048
        system, data, preboot = uuid.uuid4(), uuid.uuid4(), uuid.uuid4()
        want_data = uuid.UUID('D1B1D053-D0A6-496C-8625-A947A3DA863E')
        want_preboot = uuid.UUID('4AA4A761-5138-439D-9A30-D4138F6887FA')
        with tempfile.TemporaryDirectory() as tmp:
            image = Path(tmp) / 'scratch.img'
            with open(image, 'wb') as f:
                f.truncate(disk_sectors * gpt.SECTOR)
            fd = os.open(image, os.O_RDWR)
            try:
                entry = (
                    gpt.APFS,
                    uuid.uuid4(),
                    compose.START_SECTOR,
                    compose.START_SECTOR + part_sectors - 1,
                    'compose',
                )
                for lba, block in gpt.build(disk_sectors, [entry]):
                    os.pwrite(fd, block, lba * gpt.SECTOR)
                base = compose.START_SECTOR * gpt.SECTOR

                def obj(oid: int, xid: int, kind: int, body: bytes) -> bytes:
                    blk = bytearray(bs)
                    struct.pack_into('<QQII', blk, 8, oid, xid, kind, 0)
                    blk[32 : 32 + len(body)] = body
                    return apfs.with_checksum(bytes(blk))

                vols = [
                    (system, 1, b'System'),
                    (data, 0x40, b'SwitchDataTrial'),
                    (preboot, 0x10, b'SwitchPrebootTrial'),
                ]
                fs_oids = [1026 + i for i in range(len(vols))]
                leaf = bytearray(bs)
                struct.pack_into('<QQII', leaf, 8, 11, 5, 0x40000003, 0xB)
                struct.pack_into(
                    '<HHI', leaf, 32, apfs.BTNODE_ROOT | apfs.BTNODE_LEAF | apfs.BTNODE_FIXED_KV_SIZE, 0, len(vols)
                )
                struct.pack_into('<HH', leaf, 40, 0, 4 * len(vols))
                keys, value_end = 56 + 4 * len(vols), bs - 40
                blocks = {}
                for i, (vol_uuid, role, name) in enumerate(vols):
                    struct.pack_into('<HH', leaf, 56 + 4 * i, 16 * i, 16 * (i + 1))
                    struct.pack_into('<QQ', leaf, keys + 16 * i, fs_oids[i], 5)
                    struct.pack_into('<IIQ', leaf, value_end - 16 * (i + 1), 0, bs, 20 + i)
                    sb = bytearray(bs - 32)
                    sb[0:4] = apfs.APFS_MAGIC
                    sb[apfs.VOL_UUID_OFFSET - 32 : apfs.VOL_UUID_OFFSET - 16] = vol_uuid.bytes
                    struct.pack_into('<H', sb, apfs.VOL_ROLE_OFFSET - 32, role)
                    sb[apfs.VOL_NAME_OFFSET - 32 : apfs.VOL_NAME_OFFSET - 32 + len(name)] = name
                    blocks[20 + i] = obj(fs_oids[i], 5, apfs.OBJECT_TYPE_FS, bytes(sb))
                blocks[11] = apfs.with_checksum(bytes(leaf))
                omap = bytearray(bs - 32)
                struct.pack_into('<Q', omap, 48 - 32, 11)
                blocks[10] = obj(10, 5, apfs.OBJECT_TYPE_OMAP, bytes(omap))
                nx = bytearray(bs - 32)
                nx[0:4] = apfs.NX_MAGIC
                struct.pack_into('<IQ', nx, 4, bs, part_blocks)
                struct.pack_into('<II', nx, 104 - 32, 4, 0)
                struct.pack_into('<Q', nx, 112 - 32, 1)
                struct.pack_into('<Q', nx, 160 - 32, 10)
                struct.pack_into('<I', nx, 180 - 32, 4)
                for i, oid in enumerate(fs_oids):
                    struct.pack_into('<Q', nx, 184 - 32 + 8 * i, oid)
                blocks[0] = blocks[1] = obj(1, 5, apfs.OBJECT_TYPE_NX_SUPERBLOCK, bytes(nx))
                for paddr, data_blk in blocks.items():
                    os.pwrite(fd, data_blk, base + paddr * bs)
            finally:
                os.close(fd)
            spec = {
                'volumes': {
                    'system': {'uuid': str(system).upper(), 'role': 1},
                    'data': {
                        'uuid': str(want_data).upper(),
                        'role': 0x40,
                        'name': 'SwitchDataTrial',
                        'rocompat_features': 0x0C,
                        'incompat_features': 0x200,
                    },
                    'preboot': {'uuid': str(want_preboot).upper(), 'role': 0x10, 'name': 'SwitchPrebootTrial'},
                }
            }
            offset = compose._set_uuids(image, part_bytes, spec, mock.Mock())
            self.assertEqual(offset, compose.START_SECTOR * gpt.SECTOR)
            fd = os.open(image, os.O_RDONLY)
            try:
                c = apfs.Container(fd, offset)
                data_sb = c.read(21)
                self.assertEqual(uuid.UUID(bytes=data_sb[apfs.VOL_UUID_OFFSET : apfs.VOL_UUID_OFFSET + 16]), want_data)
                self.assertEqual(struct.unpack_from('<QQ', data_sb, apfs.VOL_ROCOMPAT_OFFSET), (0x0C, 0x200))
                self.assertEqual(struct.unpack_from('<QQ', c.read(20), apfs.VOL_ROCOMPAT_OFFSET), (0, 0))
                self.assertEqual(struct.unpack_from('<QQ', c.read(22), apfs.VOL_ROCOMPAT_OFFSET), (0, 0))
            finally:
                os.close(fd)


if __name__ == '__main__':
    unittest.main()
