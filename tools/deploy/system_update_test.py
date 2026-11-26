"""The System cryptex update's plan and write, on plain directories standing in for the card's FAT and Preboot."""

import argparse
import hashlib
import json
import tempfile
import unittest
from pathlib import Path

from tools.deploy import system_update
from tools.install import card as cards

SYSTEM = '/private/preboot/Switch-Staging/141-10410-227.dmg'
SPEC = {
    'cryptexes': [
        {'path': SYSTEM, 'member': 'system_cryptex', 'volume_uuid': 'ADB8FD34-DA7E-4538-9AAF-4F05EBB09057'},
        {'path': '/private/preboot/Switch-Staging/141-08824-244.dmg', 'member': 'app_cryptex', 'volume_uuid': 'A'},
    ],
}
ROOM = 4 << 30


class Log:
    def __init__(self) -> None:
        self.lines = []

    def __call__(self, *parts: object) -> None:
        self.lines.append(' '.join(map(str, parts)))

    quiet = __call__


def tree(root: Path) -> dict[str, bytes]:
    return {p.relative_to(root).as_posix(): p.read_bytes() for p in sorted(root.rglob('*')) if p.is_file()}


class SystemUpdateTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.preboot = self.root / 'preboot'
        staging = self.preboot / 'Switch-Staging'
        staging.mkdir(parents=True)
        (staging / '141-10410-227.dmg').write_bytes(b'o' * 8192)
        (staging / '141-10410-227.dmg.boot-prefetch').write_bytes(b'runs')
        (staging / '141-08824-244.dmg').write_bytes(b'a' * 4096)
        (self.preboot / 'cryptex1').mkdir()
        (self.preboot / 'cryptex1/current').write_bytes(b'apple')
        self.build = self.root / 'build'
        self.build.mkdir()
        self.image = self.build / '141-10410-227.dmg'
        self.image.write_bytes(b'n' * 12288)
        self.built = system_update.Image(self.image, hashlib.sha256(self.image.read_bytes()).hexdigest())

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_only_the_staged_system_image_changes(self) -> None:
        before = tree(self.preboot)
        update = system_update.plan(self.preboot, SPEC, self.built, ROOM, Log())
        self.assertFalse(update.same)
        self.assertEqual(
            (update.relative, update.size, update.card_size), ('Switch-Staging/141-10410-227.dmg', 12288, 8192)
        )
        system_update.write(self.preboot, update, self.built, Log())
        after = tree(self.preboot)
        self.assertEqual(after.pop('Switch-Staging/141-10410-227.dmg'), self.image.read_bytes())
        before.pop('Switch-Staging/141-10410-227.dmg')
        self.assertEqual(after, before)
        self.assertEqual((self.preboot / update.relative).stat().st_mode & 0o777, 0o644)

    def test_the_same_image_is_not_written(self) -> None:
        (self.preboot / 'Switch-Staging/141-10410-227.dmg').write_bytes(self.image.read_bytes())
        self.assertTrue(system_update.plan(self.preboot, SPEC, self.built, ROOM, Log()).same)

    def test_a_same_sized_other_image_is_replaced(self) -> None:
        (self.preboot / 'Switch-Staging/141-10410-227.dmg').write_bytes(b'o' * 12288)
        self.assertFalse(system_update.plan(self.preboot, SPEC, self.built, ROOM, Log()).same)

    def test_refuses_without_room_for_both_images(self) -> None:
        with self.assertRaisesRegex(cards.RefusedError, 'beside the old one'):
            system_update.plan(self.preboot, SPEC, self.built, 12288, Log())

    def test_refuses_a_card_without_the_staged_image(self) -> None:
        (self.preboot / 'Switch-Staging/141-10410-227.dmg').unlink()
        with self.assertRaisesRegex(cards.RefusedError, 'install the image first'):
            system_update.plan(self.preboot, SPEC, self.built, ROOM, Log())

    def test_refuses_an_image_under_another_name(self) -> None:
        other = self.build / '141-99999-999.dmg'
        other.write_bytes(self.image.read_bytes())
        with self.assertRaisesRegex(cards.RefusedError, 'the kernel opens'):
            system_update.plan(self.preboot, SPEC, system_update.Image(other, self.built.sha256), ROOM, Log())

    def test_refuses_a_translation_the_boot_package_does_not_trust(self) -> None:
        trusted, pins = [], {}
        for name in system_update.TRUSTED:
            trusted.append(self.build / name)
            trusted[-1].write_bytes(name.encode())
            pins[name] = hashlib.sha256(name.encode()).hexdigest()
        pins['os.trustcache'] = 'not this role'
        system_update.check_translation(trusted, pins)
        with self.assertRaisesRegex(cards.RefusedError, 'system_cryptex.trustcache'):
            system_update.check_translation(trusted[1:], pins)
        trusted[1].write_bytes(b'a cache list with one more cdhash')
        with self.assertRaisesRegex(cards.RefusedError, 'a64x-cache-pretranslated.bin'):
            system_update.check_translation(trusted, pins)

    def test_refuses_a_card_whose_boot_entry_is_not_this_builds(self) -> None:
        package, fat = self.root / 'package', self.root / 'fat'
        for root, bl33 in ((package, b'new'), (fat, b'old')):
            (root / 'switchroot/ios').mkdir(parents=True)
            (root / 'switchroot/ios/bl31.bin').write_bytes(b'atf')
            (root / 'switchroot/ios/bl33.bin').write_bytes(bl33)
            (root / 'bootloader/ini').mkdir(parents=True)
            (root / 'bootloader/ini/ios.ini').write_text('[iOS]\n')
        (fat / 'switchroot/ios/bootargs.txt').write_text('kextlog=0x77\n')
        before = tree(fat)
        with self.assertRaisesRegex(cards.RefusedError, r'switchroot/ios/bl33\.bin'):
            system_update.check_boot_entry(fat, package, Log())
        self.assertEqual(tree(fat), before)
        (fat / 'switchroot/ios/bl33.bin').write_bytes(b'new')
        system_update.check_boot_entry(fat, package, Log())

    def test_refuses_a_kind_with_no_translated_system(self) -> None:
        pins = self.root / 'ipsw.json'
        pins.write_text(json.dumps({'version': '17.7.11', 'build': '21H461'}))
        args = argparse.Namespace(ipsw=pins, image=None, trusted=[], pretranslated_pins=None)
        with self.assertRaisesRegex(cards.RefusedError, r'17\.7\.11 \(21H461\).*nothing to update'):
            system_update._inputs(args)


if __name__ == '__main__':
    unittest.main()
