"""The container fit against macOS itself.

A container grown to the nominal size and fitted to partitions of other sizes must attach in the kernel at that size
and pass fsck_apfs; a fit below the minimum must change nothing. A container APFS has written and freed thousands of
extents in, whose main free queue is a root over leaves (what a populated image holds when its last transactions freed
more than one leaf takes), must fit the same way.

Manual and local: hdiutil and diskutil on sparse images (about 1 TiB each, a few MB on disk), no root.
"""

import os
import plistlib
import shutil
import struct
import subprocess
import tempfile
import unittest
import uuid
from pathlib import Path

from python.runfiles import runfiles

from tools.install import apfs, disks, gpt

BLOCK = 4096
NOMINAL = 1 << 40
FIRST = 2048
GIB = 1 << 30
CHECKPOINT_MAP = 0xC
SM_FQ, SM_FQ_BYTES, MAIN_QUEUE = 200, 40, 1
# Written, then every other one deleted, so the freed extents cannot merge: far more than one free queue leaf holds.
CHURN_FILES = 6000


def run(*args: str) -> str:
    return subprocess.run(args, capture_output=True, text=True, check=True).stdout


def attach(image: Path) -> str:
    out = plistlib.loads(
        subprocess.run(
            [
                '/usr/bin/hdiutil',
                'attach',
                '-plist',
                '-nomount',
                '-imagekey',
                'diskimage-class=CRawDiskImage',
                str(image),
            ],
            capture_output=True,
            check=True,
        ).stdout
    )
    return min(e['dev-entry'] for e in out['system-entities'])


def container_size(disk: str) -> int | None:
    listing = plistlib.loads(
        subprocess.run(['/usr/sbin/diskutil', 'apfs', 'list', '-plist'], capture_output=True, check=False).stdout
    )
    name = disk.removeprefix('/dev/')
    for c in listing.get('Containers', []):
        stores = [s.get('DeviceIdentifier', '') for s in c['PhysicalStores']]
        if any(store == name or store.startswith(name + 's') for store in stores):
            return c.get('CapacityCeiling')
    return None


def tool() -> str:
    return runfiles.Create().Rlocation('_main/boot/installer/apfs_tool')


def measure(image: Path, offset: int = 0) -> tuple[int, int]:
    size, minimum, _, _ = map(int, run(tool(), str(image), str(offset)).split())
    return size, minimum


def main_queue(image: Path, offset: int) -> tuple[int, int, int]:
    """Return the main free queue's entries, its tree's root level and the root's entry count."""
    fd = os.open(image, os.O_RDONLY)
    try:
        container = apfs.Container(fd, offset)
        nx = container.superblocks()[0]
        desc_blocks, desc_base = struct.unpack_from('<I', nx, 104)[0], struct.unpack_from('<Q', nx, 112)[0]
        index, length = struct.unpack_from('<II', nx, 136)
        ephemeral = {}
        for j in range(length):
            block = container.read(desc_base + (index + j) % desc_blocks)
            if apfs.object_header(block)[2] == CHECKPOINT_MAP:
                for k in range(struct.unpack_from('<I', block, 36)[0]):
                    oid, paddr = struct.unpack_from('<QQ', block, 40 + 40 * k + 24)
                    ephemeral[oid] = paddr
        spaceman = container.read(ephemeral[struct.unpack_from('<Q', nx, 152)[0]])
        entries, tree = struct.unpack_from('<QQ', spaceman, SM_FQ + MAIN_QUEUE * SM_FQ_BYTES)
        level, keys = struct.unpack_from('<HI', container.read(ephemeral[tree]), 34)
        return entries, level, keys
    finally:
        os.close(fd)


class Quiet:
    def __call__(self, *_parts: object) -> None:
        pass

    def quiet(self, *_parts: object) -> None:
        pass


def churn(root: Path) -> None:
    """Write CHURN_FILES files of one to three blocks and delete every other one, as one burst before unmounting."""
    files = root / 'files'
    files.mkdir()
    for i in range(CHURN_FILES):
        (files / f'{i:06d}').write_bytes(bytes([i & 0xFF]) * BLOCK * (1 + i % 3))
    os.sync()
    for i in range(0, CHURN_FILES, 2):
        (files / f'{i:06d}').unlink()


class FitTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tool = tool()
        cls.dir = Path(tempfile.mkdtemp())
        cls.nominal = cls.dir / 'nominal.img'
        with open(cls.nominal, 'wb') as f:
            f.truncate(NOMINAL + 2 * GIB)
        disk = attach(cls.nominal)
        try:
            run('/usr/sbin/diskutil', 'partitionDisk', disk, 'GPT', 'APFS', 'fit', '6G', 'free', 'free', 'R')
            store = disk + 's2'
            run('/usr/sbin/diskutil', 'apfs', 'resizeContainer', store, f'{NOMINAL}B')
        finally:
            run('/usr/bin/hdiutil', 'detach', disk)
        fd = os.open(cls.nominal, os.O_RDONLY)
        cls.offset = gpt.read(fd, os.fstat(fd).st_size).used[-1].first * 512
        os.close(fd)
        cls.size, cls.minimum = measure(cls.nominal, cls.offset)
        cls.churned = cls.dir / 'churned.img'
        subprocess.run(['/bin/cp', '-c', str(cls.nominal), str(cls.churned)], check=True)
        log = Quiet()
        with disks.attached(cls.churned, log, raw=True) as image, disks.mounted(image.volumes[0], log, False) as root:
            churn(root)

    @classmethod
    def tearDownClass(cls) -> None:
        shutil.rmtree(cls.dir)

    def fitted(self, blocks: int, source: Path) -> Path:
        """Make a partition of exactly `blocks` holding `source`'s container fitted to it."""
        copy = self.dir / 'copy.img'
        subprocess.run(['/bin/cp', '-c', str(source), str(copy)], check=True)
        run(self.tool, str(copy), str(self.offset), str(blocks))
        out = self.dir / f'{blocks}.img'
        sectors = FIRST + blocks * 8 + 2048
        with open(out, 'wb') as f:
            f.truncate(sectors * 512)
        src, dst = os.open(copy, os.O_RDONLY), os.open(out, os.O_RDWR)
        for lba, data in gpt.build(sectors, [(gpt.APFS, uuid.uuid4(), FIRST, FIRST + blocks * 8 - 1, 'iOS')]):
            os.pwrite(dst, data, lba * 512)
        at = 0
        while at < blocks * BLOCK:
            try:
                data = os.lseek(src, self.offset + at, os.SEEK_DATA) - self.offset
            except OSError:
                break
            if data >= blocks * BLOCK:
                break
            hole = min(os.lseek(src, self.offset + data, os.SEEK_HOLE) - self.offset, blocks * BLOCK)
            os.pwrite(dst, os.pread(src, hole - data, self.offset + data), FIRST * 512 + data)
            at = hole
        os.close(src)
        os.close(dst)
        copy.unlink()
        return out

    def check(self, blocks: int, source: Path | None = None) -> None:
        image = self.fitted(blocks, source or self.nominal)
        disk = attach(image)
        try:
            self.assertEqual(container_size(disk), blocks * BLOCK)
            store = disk.removeprefix('/dev/') + 's1'
            subprocess.run(['/usr/sbin/diskutil', 'unmountDisk', store], capture_output=True, check=False)
            fsck = subprocess.run(
                ['/sbin/fsck_apfs', '-n', '/dev/r' + store], capture_output=True, text=True, check=False
            )
            self.assertIn('appears to be OK', fsck.stdout + fsck.stderr)
        finally:
            run('/usr/bin/hdiutil', 'detach', disk)
            image.unlink()

    def test_the_nominal_size(self):
        self.assertEqual(self.size * BLOCK, NOMINAL)
        self.assertLess(self.minimum * BLOCK, 16 * GIB)

    def test_fits_on_both_sides_of_the_pool_queue_limit(self):
        for size in (16 * GIB, 100 * GIB + 12 * BLOCK, 500 * GIB, NOMINAL - 7 * 32768 * BLOCK):
            with self.subTest(size=size):
                self.check(size // BLOCK)

    def test_the_churned_containers_main_free_queue_is_split(self):
        entries, level, keys = main_queue(self.churned, self.offset)
        self.assertEqual(level, 1, 'the queue must be a root over leaves for the churned fits to mean anything')
        self.assertGreater(keys, 1)
        self.assertGreater(entries, CHURN_FILES // 4)

    def test_a_churned_container_fits(self):
        size, minimum = measure(self.churned, self.offset)
        self.assertEqual(size, self.size)
        for blocks in (max(minimum, 16 * GIB // BLOCK), 100 * GIB // BLOCK):
            with self.subTest(blocks=blocks):
                self.check(blocks, self.churned)

    def test_below_the_minimum_nothing_is_written(self):
        result = subprocess.run(
            [self.tool, str(self.nominal), str(self.offset), str(self.minimum - 1)], capture_output=True, check=False
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(
            run(self.tool, str(self.nominal), str(self.offset)).split()[:2], [str(self.size), str(self.minimum)]
        )


if __name__ == '__main__':
    unittest.main()
