"""compose's attaches: an OS image's APFS physical store, and an attached image's container.

_attach_os finds the store as hdiutil attach and diskutil list it. os_measure and _grow read the container by the
container disk attach names, which answers while diskutil's whole APFS listing still lags behind the attach.
"""

import contextlib
import unittest
from pathlib import Path
from unittest import mock

from tools.install import compose

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


if __name__ == '__main__':
    unittest.main()
