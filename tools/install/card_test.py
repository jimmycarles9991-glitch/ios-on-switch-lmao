"""The card guards on the real partition table and on broken variants of it (no device, no diskutil)."""

import base64
import json
import os
import tempfile
import unittest
import uuid
import zlib
from collections.abc import Callable
from pathlib import Path
from unittest import mock

from python.runfiles import runfiles

from tools.install import card as cards, gpt

SPEC = {'partition_name': 'iOS'}


def table(mutate: Callable[[bytearray, bytearray], None] | None = None):
    layout = json.loads(Path(runfiles.Create().Rlocation('_main/install/testdata/switch-card-1tb.json')).read_text())
    sectors = {r['lba']: bytearray(base64.b64decode(r['base64'])) for r in layout['sectors']}
    disk_sectors = layout['disk_bytes'] // 512
    if mutate:
        entries = sectors[2]
        mutate(entries, sectors[0])
        crc = zlib.crc32(bytes(entries[: 10 * 128]))
        sectors[disk_sectors - 33][:] = entries
        for lba in (1, disk_sectors - 1):
            h = sectors[lba]
            h[88:92] = crc.to_bytes(4, 'little')
            h[16:20] = bytes(4)
            h[16:20] = zlib.crc32(bytes(h[:92])).to_bytes(4, 'little')
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, 'card.img')
        with open(path, 'wb') as f:
            f.truncate(layout['disk_bytes'])
            for lba, data in sectors.items():
                f.seek(lba * 512)
                f.write(data)
        fd = os.open(path, os.O_RDONLY)
        try:
            t = gpt.read(fd, layout['disk_bytes'])
        finally:
            os.close(fd)
    return cards.Card('disk99', {}, layout['disk_bytes'], '', t)


def retype(index: int, kind: uuid.UUID):
    def mutate(entries: bytearray, _mbr: bytearray) -> None:
        entries[index * 128 : index * 128 + 16] = kind.bytes_le

    return mutate


def both(*mutations: Callable[[bytearray, bytearray], None]):
    def mutate(entries: bytearray, mbr: bytearray) -> None:
        for m in mutations:
            m(entries, mbr)

    return mutate


def rename(index: int, name: str):
    def mutate(entries: bytearray, _mbr: bytearray) -> None:
        entries[index * 128 + 56 : index * 128 + 128] = name.encode('utf-16-le').ljust(72, b'\0')

    return mutate


def shrink(index: int, sectors: int):
    def mutate(entries: bytearray, _mbr: bytearray) -> None:
        last = int.from_bytes(entries[index * 128 + 40 : index * 128 + 48], 'little') - sectors
        entries[index * 128 + 40 : index * 128 + 48] = last.to_bytes(8, 'little')

    return mutate


class CardTest(unittest.TestCase):
    def setUp(self):
        patcher = mock.patch.object(cards.host, 'disk_info', return_value={'FilesystemType': 'msdos'})
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_linux_and_android_card_asks_which(self):
        with self.assertRaisesRegex(cards.RefusedError, '--partition'):
            cards.check_layout(table(), SPEC)
        target = cards.check_layout(table(), SPEC, partition='userdata')
        self.assertEqual((target.index, target.name), (8, 'userdata'))
        self.assertEqual(cards.check_layout(table(), SPEC, partition='l4t').index, 1)

    def test_one_fresh_partition_is_taken(self):
        target = cards.check_layout(table(rename(1, 'linux')), SPEC)
        self.assertEqual((target.index, target.name), (8, 'userdata'))

    def test_already_installed_card(self):
        card = table(retype(8, gpt.APFS))
        self.assertEqual(cards.check_layout(card, SPEC).index, 8)
        self.assertEqual(cards.check_layout(table(rename(1, 'iOS')), SPEC).index, 1)
        self.assertEqual(cards.check_layout(table(rename(8, 'iOS-setup')), SPEC).index, 8)

    def test_any_aligned_size(self):
        self.assertEqual(cards.check_layout(table(both(rename(1, 'linux'), shrink(8, 8))), SPEC).index, 8)
        with self.assertRaisesRegex(cards.RefusedError, 'cannot hold iOS'):
            cards.check_layout(table(both(rename(1, 'linux'), shrink(8, 1))), SPEC)

    def test_refuses_second_apfs_partition(self):
        with self.assertRaisesRegex(cards.RefusedError, 'more than one APFS'):
            cards.check_layout(table(both(retype(1, gpt.APFS), retype(8, gpt.APFS))), SPEC)
        with self.assertRaisesRegex(cards.RefusedError, 'already has iOS'):
            cards.check_layout(table(retype(1, gpt.APFS)), SPEC, partition='userdata')

    def test_hybrid_mbr_may_list_the_whole_target(self):
        card = table()
        l4t = card.table.entries[1]
        card.table.mbr_partitions.append(gpt.MbrPartition(3, 0, cards.LINUX_MBR, l4t.first, l4t.sectors))
        self.assertEqual(cards.check_layout(card, SPEC, partition='l4t').index, 1)
        card.table.mbr_partitions[-1] = gpt.MbrPartition(3, 0, cards.LINUX_MBR, l4t.first, l4t.sectors - 8)
        with self.assertRaisesRegex(cards.RefusedError, 'hybrid MBR'):
            cards.check_layout(card, SPEC, partition='l4t')

    def test_refuses_without_hos_data(self):
        def no_fat(entries: bytearray, _mbr: bytearray) -> None:
            entries[0:16] = gpt.LINUX_DATA.bytes_le

        with self.assertRaisesRegex(cards.RefusedError, 'hos_data'):
            cards.check_layout(table(no_fat), SPEC)

    def test_refuses_without_hybrid_mbr(self):
        def protective(entries: bytearray, mbr: bytearray) -> None:
            pass

        card = table(protective)
        card.table.mbr_partitions = [p for p in card.table.mbr_partitions if p.kind == 0xEE]
        with self.assertRaisesRegex(cards.RefusedError, 'hybrid MBR'):
            cards.check_layout(card, SPEC)

    def test_refuses_emummc_or_unknown_target(self):
        def unknown(entries: bytearray, _mbr: bytearray) -> None:
            entries[8 * 128 + 16 : 8 * 128 + 32] = bytes(15) + b'\1'

        with self.assertRaisesRegex(cards.RefusedError, 'expected one Linux'):
            cards.check_layout(table(both(rename(1, 'linux'), rename(8, 'other'), unknown)), SPEC)
        with self.assertRaisesRegex(cards.RefusedError, 'expected one Linux'):
            cards.check_layout(table(both(rename(1, 'linux'), retype(8, gpt.EMUMMC))), SPEC)

    def test_a_retype_may_change_only_its_entry_and_the_headers(self):
        card = table()
        span = card.table.entry_sectors * gpt.SECTOR
        before = bytes((3 + 2 * card.table.entry_sectors) * gpt.SECTOR)

        def changed(*offsets: int, index: int | None = 8) -> list[int]:
            after = bytearray(before)
            for at in offsets:
                after[at] = 1
            return cards.changed_outside(card, before, bytes(after), index)

        entry = 2 * gpt.SECTOR + 8 * 128
        allowed = (gpt.SECTOR, entry, entry + 127, entry + span + 56, len(before) - 1)
        self.assertEqual(changed(*allowed), [])
        self.assertEqual(changed(*allowed, index=None), list(allowed))
        self.assertEqual(changed(446, entry - 1, entry + 128 + span), [446, entry - 1, entry + 128 + span])
        with self.assertRaisesRegex(cards.host.HostError, 'shape'):
            cards.changed_outside(card, before, before[:-1], 8)

    def test_resolve_refuses_partitions_and_paths(self):
        for argument in ('disk4s9', '/Volumes/SWITCH SD', '/dev/disk4s1', 'rdisk4s2'):
            with self.assertRaises(cards.RefusedError):
                cards.resolve(argument)


class MountedTest(unittest.TestCase):
    def mount(self, info: dict, owners: bool = False, taken: list[str] | None = None) -> list[list[str]]:
        """`taken`: where macOS mounts the volume itself, one per `diskutil mount`, before ours lands."""
        calls = []
        mounted_at = ['/Volumes/Data']
        taken = list(taken or [])

        def run(*args: str, **_: object) -> str:
            calls.append(list(args))
            if args[:2] == ('diskutil', 'unmount'):
                mounted_at[0] = None
            if args[:2] == ('diskutil', 'mount'):
                mounted_at[0] = taken.pop(0) if taken else args[-2]
            return ''

        with (
            mock.patch.object(cards.host, 'mount_point', side_effect=lambda _: mounted_at[0]),
            mock.patch.object(cards.host, 'disk_info', return_value=info),
            mock.patch.object(cards.host, 'run', side_effect=run),
            cards.mounted('disk34s2', lambda *_: None, owners=owners) as root,
        ):
            calls.append(['root', str(root)])
        return calls

    def test_a_mount_that_ignores_owners_is_reused(self):
        calls = self.mount({'WritableVolume': True, 'GlobalPermissionsEnabled': False})
        self.assertEqual(calls, [['root', '/Volumes/Data']])

    def test_a_mount_that_enforces_owners_is_remounted_without_them(self):
        calls = self.mount({'WritableVolume': True, 'GlobalPermissionsEnabled': True})
        self.assertEqual(calls[0], ['diskutil', 'unmount', 'disk34s2'])
        self.assertEqual(calls[1][:3], ['diskutil', 'mount', '-mountOptions'])
        self.assertEqual(calls[1][3], 'nobrowse,noowners')
        self.assertNotEqual(calls[2], ['root', '/Volumes/Data'])

    def test_owners_wanted_keeps_the_mount(self):
        calls = self.mount({'WritableVolume': True, 'GlobalPermissionsEnabled': True}, owners=True)
        self.assertEqual(calls, [['root', '/Volumes/Data']])

    def test_a_volume_macos_mounts_first_is_taken_back(self):
        calls = self.mount({'WritableVolume': True, 'GlobalPermissionsEnabled': True}, taken=['/Volumes/SWITCH SD'])
        mounts = [i for i, c in enumerate(calls) if c[:2] == ['diskutil', 'mount']]
        self.assertEqual(len(mounts), 2)
        self.assertEqual(calls[mounts[0] + 1], ['diskutil', 'unmount', 'disk34s2'])
        self.assertEqual(calls[mounts[1] + 1], ['root', calls[mounts[1]][-2]])

    def test_a_volume_that_never_mounts_here_is_refused(self):
        with self.assertRaises(cards.host.HostError):
            self.mount({'WritableVolume': True, 'GlobalPermissionsEnabled': True}, taken=['/Volumes/SWITCH SD'] * 3)


if __name__ == '__main__':
    unittest.main()
