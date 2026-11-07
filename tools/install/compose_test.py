"""compose._attach_os: the APFS physical store of an OS image, as hdiutil attach and diskutil list it."""

import unittest
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
    def attach(self, entities: list[dict], store: str) -> tuple[str, str]:
        stores = {'/dev/disk33': {'APFSPhysicalStores': [{'APFSPhysicalStore': store}]}}
        with (
            mock.patch.object(compose.host, 'plist', return_value={'system-entities': entities}),
            mock.patch.object(compose.host, 'disk_info', side_effect=stores.get),
        ):
            return compose._attach_os('os.dmg', log=None)

    def test_a_gpt_image_gives_its_apfs_partition_and_the_disk_around_it(self) -> None:
        self.assertEqual(self.attach(GPT_IMAGE, 'disk32s1'), ('/dev/disk32', '/dev/disk32s1'))

    def test_a_bare_container_image_is_its_own_store(self) -> None:
        self.assertEqual(self.attach(BARE_CONTAINER, 'disk32'), ('/dev/disk32', '/dev/disk32'))

    def test_the_store_does_not_depend_on_how_hdiutil_hints_the_image(self) -> None:
        self.assertEqual(self.attach(BARE_CONTAINER_UNHINTED, 'disk32'), ('/dev/disk32', '/dev/disk32'))

    def test_no_synthesized_container_is_refused(self) -> None:
        with (
            mock.patch.object(compose.host, 'plist', return_value={'system-entities': GPT_IMAGE[:2]}),
            mock.patch.object(compose.subprocess, 'run'),
            self.assertRaises(compose.host.HostError),
        ):
            compose._attach_os('os.dmg', log=None)


if __name__ == '__main__':
    unittest.main()
