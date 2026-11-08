"""The direct install onto a disk image built like the card (testcard.py), twice.

First onto userdata, then over the iOS container the first run left attached, as on a card that already has iOS. The
image is a real APFS container composed for 1 TiB like //:image's and packed by pack.py, so the run fits it to
the 32 GiB partition with apfs_tool. Needs hdiutil, newfs_apfs, fsck_apfs (macOS) and disk images this user attaches:
bazel test --strategy=TestRunner=local //tools/install:direct_card_test.
"""

import hashlib
import os
import shutil
import subprocess
import tempfile
import time
import unittest
import uuid
from pathlib import Path

from python.runfiles import runfiles

from tools.install import card as cards, compose, direct, folder, gpt, host, pack, testcard

NOMINAL = 1 << 40
START = 2048
MIB, GIB = 1 << 20, 1 << 30
MARK = b'not iOS: keep me '
BOOT = {'bl31.bin': b'firmware' * 4096, 'bl33.bin': os.urandom(3 * MIB + 512)}
SHARED = {cards.INI: b'[iOS]\nicon=bootloader/res/icon_ios_hue.bmp\n', cards.ICON: b'BM' + bytes(4096)}


class Log:
    path = Path(os.devnull)

    def __call__(self, *parts: object) -> None:
        print(*parts, flush=True)

    def quiet(self, *parts: object) -> None:
        pass


def run(*command: str) -> str:
    return subprocess.run(command, check=True, capture_output=True, text=True).stdout


def container_image(work: Path) -> tuple[Path, dict, bytes]:
    """Make a raw image of one 1 TiB APFS container holding a payload; return it, its measure and the payload."""
    image, payload = work / 'container.img', os.urandom(96 * MIB)
    sectors = START + NOMINAL // 512 + 2048
    with open(image, 'wb') as f:
        f.truncate(sectors * 512)
        for lba, data in gpt.build(sectors, [(gpt.APFS, uuid.uuid4(), START, START + NOMINAL // 512 - 1, 'c')]):
            f.seek(lba * 512)
            f.write(data)
    disk = compose.attach_raw(image, Log())
    try:
        run('/sbin/newfs_apfs', '-v', 'Data', f'/dev/r{disk}s1')
        mount = work / 'mnt'
        mount.mkdir()
        volume = wait_for_container(disk + 's1')['Volumes'][0]['DeviceIdentifier']
        run('/usr/sbin/diskutil', 'mount', '-mountPoint', str(mount), volume)
        (mount / 'payload').write_bytes(payload)
        (mount / 'dir').mkdir()
        for i in range(200):
            (mount / 'dir' / f'file{i}').write_text(f'small {i}\n')
        run('/usr/sbin/diskutil', 'unmountDisk', volume)
    finally:
        run('/usr/bin/hdiutil', 'detach', disk)
    tool = apfs_tool()
    blocks, minimum, pool, pool_blocks = map(int, run(tool, str(image), str(START * 512)).split())
    layout = {
        'container_bytes': blocks * 4096,
        'minimum_bytes': max(16 * GIB, minimum * 4096 + 4 * GIB),
        'pool_start': pool * 4096,
        'pool_end': (pool + pool_blocks) * 4096,
    }
    return image, layout, payload


def wait_for_container(store: str) -> dict:
    for _ in range(60):
        found = host.container_on(store)
        if found and found.get('Volumes'):
            return found
        time.sleep(0.5)
    raise AssertionError(f'no container attached on {store}')


def apfs_tool() -> str:
    return runfiles.Create().Rlocation('_main/boot/installer/apfs_tool')


def layout_json() -> str:
    return runfiles.Create().Rlocation('_main/install/testdata/switch-card-1tb.json')


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class DirectCardTest(unittest.TestCase):
    def setUp(self):
        self.work = Path(tempfile.mkdtemp(prefix='switch-direct-'))
        self.addCleanup(shutil.rmtree, self.work, True)
        self.out = self.work / 'sd'
        boot = []
        for name, data in BOOT.items():
            (self.work / name).write_bytes(data)
            boot.append((name, self.work / name))
        image, layout, self.payload = container_image(self.work)
        folder.prepare(self.out)
        target = pack.Target(self.out / folder.SII_DIR, folder.SII_DIR, 'IOS99', 'iPadOS test', boot)
        self.m = pack.pack(pack.Source(image, START * 512, layout), target, Log())
        image.unlink()
        for relative, data in SHARED.items():
            (self.out / relative).parent.mkdir(parents=True, exist_ok=True)
            (self.out / relative).write_bytes(data)
        self.card = self.work / 'card.img'
        testcard.create(str(self.card), layout_json(), 'normal')
        self.disk = testcard.attach(self.card)[len('/dev/') :]
        self.addCleanup(testcard.detach, self.card)

    def install(self, *extra: str) -> int:
        log = self.work / 'direct.log'
        args = [self.disk, '--yes', f'--confirm={self.disk}', f'--out={self.out}', '--image=IOS99']
        return direct.main([*args, f'--apfs-tool={apfs_tool()}', f'--log={log}', *extra])

    def table(self) -> cards.Card:
        card = cards.resolve(self.disk)
        cards.read_table(card)
        return card

    def mark_neighbours(self, card: cards.Card) -> dict[int, bytes]:
        """Mark the sectors either side of userdata and the first of every other partition but the FAT."""
        userdata = card.table.entries[8]
        places = [userdata.first - 1, userdata.last + 1] + [e.first for e in card.table.used if e.index not in (0, 8)]
        fd = os.open(card.raw, os.O_RDWR)
        try:
            for lba in places:
                os.pwrite(fd, MARK.ljust(512, b'.'), lba * 512)
        finally:
            os.close(fd)
        return {lba: MARK.ljust(512, b'.') for lba in places}

    def check_card(self, before: bytes, marks: dict[int, bytes]) -> None:
        card = self.table()
        target = card.table.entries[8]
        self.assertEqual((target.type, target.name), (gpt.APFS, 'iOS'))
        self.assertEqual(cards.changed_outside(card, before, cards.table_snapshot(card), 8), [])
        fd = os.open(card.raw, os.O_RDONLY)
        try:
            self.assertEqual({lba: os.pread(fd, 512, lba * 512) for lba in marks}, marks)
        finally:
            os.close(fd)
        container = wait_for_container(f'{self.disk}s9')
        self.assertEqual(container['APFSContainerUUID'].upper(), str(self.m.container_uuid).upper())
        self.assertEqual(int(container['CapacityCeiling']), target.bytes)
        with cards.mounted(container['Volumes'][0]['DeviceIdentifier'], Log(), read_only=True) as root:
            self.assertEqual(sha((root / 'payload').read_bytes()), sha(self.payload))
            self.assertEqual(len(list((root / 'dir').iterdir())), 200)
        with cards.mounted(f'{self.disk}s1', Log(), read_only=True) as fat:
            for name, data in BOOT.items():
                self.assertEqual(sha((fat / cards.BOOT_ENTRY / name).read_bytes()), sha(data))
            for relative, data in {**SHARED, **testcard.OTHER_FILES}.items():
                self.assertEqual((fat / relative).read_bytes(), data, relative)
            self.assertFalse(list((fat / cards.BOOT_ENTRY).glob('*.SII.*')))

    def test_installs_onto_userdata_and_again_over_the_attached_container(self):
        card = self.table()
        before = cards.table_snapshot(card)
        marks = self.mark_neighbours(card)
        self.assertEqual(self.install('--partition=userdata'), 0)
        self.check_card(before, marks)
        self.assertEqual(self.install(), 0)
        self.check_card(before, marks)

    def test_dry_run_and_refusals_write_nothing(self):
        before = (cards.table_snapshot(self.table()), self.card.stat().st_blocks)
        args = [self.disk, f'--out={self.out}', '--partition=userdata', f'--apfs-tool={apfs_tool()}']
        self.assertEqual(direct.main([*args, f'--log={self.work / "dry.log"}']), 0)
        self.assertEqual(direct.main([*args, '--yes', '--confirm=disk0', f'--log={self.work / "no.log"}']), 2)
        unnamed = [self.disk, f'--out={self.out}', f'--apfs-tool={apfs_tool()}', f'--log={self.work / "which.log"}']
        self.assertEqual(direct.main([*unnamed, '--yes', f'--confirm={self.disk}']), 2)
        self.m.minimum_bytes = 64 * GIB
        pack.write_head(self.out / folder.SII_DIR, self.m, [(n, self.work / n) for n in BOOT])
        self.assertEqual(direct.main([*args, f'--log={self.work / "small.log"}']), 2)
        self.assertEqual((cards.table_snapshot(self.table()), self.card.stat().st_blocks), before)


if __name__ == '__main__':
    unittest.main()
