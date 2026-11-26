"""The copy step needs only the card's FAT: a card with both a Linux and an Android partition is fine (no device)."""

import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools.install import card as cards, folder, to_card


def part(index: int, name: str, content: str, filesystem: str = 'msdos') -> cards.Part:
    return cards.Part(index, f'disk9s{index + 1}', name, '', content, 1 << 30, filesystem)


class FatTest(unittest.TestCase):
    CARD = cards.Card('disk9', {}, 1 << 36)

    def test_takes_hos_data_whatever_the_other_partitions(self):
        parts = [part(0, 'hos_data', 'Microsoft Basic Data'), part(1, 'l4t', 'Linux'), part(2, 'userdata', 'Linux')]
        with mock.patch.object(cards, 'diskutil_partitions', return_value=parts):
            self.assertEqual(to_card.fat_of(self.CARD).identifier, 'disk9s1')

    def test_exfat_and_unprobed_hos_data_are_accepted(self):
        for filesystem in ('exfat', ''):
            parts = [part(0, 'hos_data', 'Microsoft Basic Data', filesystem), part(1, 'iOS', 'Apple_APFS')]
            with (
                self.subTest(filesystem=filesystem),
                mock.patch.object(cards, 'diskutil_partitions', return_value=parts),
            ):
                self.assertEqual(to_card.fat_of(self.CARD), parts[0])
                self.assertEqual(cards.check_layout_unprivileged(self.CARD, {}), tuple(parts))

    def test_foreign_filesystem_or_wrong_slot_is_refused(self):
        for slot, filesystem in [(0, 'ntfs'), (0, 'apfs'), (1, 'exfat')]:
            with self.subTest(slot=slot, filesystem=filesystem), self.assertRaises(cards.RefusedError):
                cards.shared_partition(self.CARD, [part(slot, 'hos_data', 'Microsoft Basic Data', filesystem)])

    def test_refuses_a_card_that_is_not_a_switch_card(self):
        for parts in ([], [part(0, 'EFI', 'EFI')], [part(0, 'hos_data', 'Apple_APFS')]):
            with (
                self.subTest(parts=parts),
                mock.patch.object(cards, 'diskutil_partitions', return_value=parts),
                self.assertRaises(cards.RefusedError),
            ):
                to_card.fat_of(self.CARD)


class PlanTest(unittest.TestCase):
    def test_the_shared_files_that_differ_are_planned_and_counted(self) -> None:
        out, root = Path(tempfile.mkdtemp()), Path(tempfile.mkdtemp())
        for relative, data in ((folder.PAYLOAD, b'new installer'), (cards.INI, b'[iOS]\n')):
            (out / relative).parent.mkdir(parents=True, exist_ok=True)
            (out / relative).write_bytes(data)
        (root / cards.INI).parent.mkdir(parents=True)
        (root / cards.INI).write_bytes(b'[iOS]\n')
        lines = []
        need = to_card.plan(out, root, [], lambda *parts: lines.append(' '.join(map(str, parts))))
        self.assertEqual(lines, [f'  copy: {folder.PAYLOAD}'])
        self.assertEqual(need, len(b'new installer'))


if __name__ == '__main__':
    unittest.main()
