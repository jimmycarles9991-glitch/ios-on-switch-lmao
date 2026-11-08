"""room.fit and apfs_tool on containers newfs_apfs made, as a cryptex is, checked by macOS itself.

The kernel takes a container only when its free queue limits are exactly what its size calls for (boot/installer/
apfs.c), though fsck_apfs passes it either way. So the fits land on each side of every step of those limits: the main
queue's at 4544 and 13632 blocks, 1 GiB and 4 GiB, and the internal pool's at 1136, 2272 and 3408 pool blocks (375,
751 and 1127 chunks). Each fitted container must attach with its volume, at its new size, and pass fsck_apfs:
hdiutil, diskutil and fsck_apfs, which work inside darwin-sandbox; skipped where there is no diskutil. The argument is
apfs_tool.
"""

import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

from tools.install import compose, disks, host, room

APFS_TOOL = ''
BLOCK = 4096
GIB_BLOCKS = 1 << 18
CHUNK = 32768
# {the newfs_apfs container's size: the sizes it is fitted to}, in blocks
FITS = {
    16384: (13632,),
    GIB_BLOCKS: (13633, 4545, 4544),
    2 * GIB_BLOCKS: (GIB_BLOCKS, GIB_BLOCKS - 1),
    8 * GIB_BLOCKS: (4 * GIB_BLOCKS, 4 * GIB_BLOCKS - 1),
    256 * GIB_BLOCKS: (375 * CHUNK, 375 * CHUNK + 1, 751 * CHUNK, 751 * CHUNK + 1, 1127 * CHUNK, 1127 * CHUNK + 1),
}


class Quiet:
    def __call__(self, *_parts: object) -> None:
        pass

    def quiet(self, *_parts: object) -> None:
        pass


LOG = Quiet()


def blank(image: Path, blocks: int) -> None:
    room.blank(image, blocks * BLOCK, 'Fitted', LOG)


def check(image: Path, blocks: int) -> None:
    """Attach the raw container in `image`: its volume must appear, the container be `blocks`, fsck_apfs pass."""
    with disks.attached(image, LOG, writable=False, raw=True) as target:
        if len(target.volumes) != 1:
            raise host.HostError(f'macOS attaches no volume from {image.name}')
        container = host.container_on(target.whole) or {}
        if container.get('CapacityCeiling') != blocks * BLOCK:
            raise host.HostError(f'the container is {container.get("CapacityCeiling")} bytes, not {blocks * BLOCK}')
        compose.fsck(target.whole, image.name, LOG)


@unittest.skipUnless(os.path.exists('/usr/sbin/diskutil'), 'needs macOS: hdiutil, diskutil and fsck_apfs')
class Fit(unittest.TestCase):
    def setUp(self) -> None:
        self.dir = Path(tempfile.mkdtemp(prefix='fit-'))

    def tearDown(self) -> None:
        disks.sweep(self.dir, LOG)
        shutil.rmtree(self.dir)

    def test_room_fit_shrinks_a_newfs_container(self):
        image = self.dir / 'cryptex.img'
        blank(image, 2 * GIB_BLOCKS)
        fitted = room.fit(image, APFS_TOOL, lambda clone: check(clone, os.path.getsize(clone) // BLOCK), LOG, 'test')
        self.assertLess(fitted, 64 << 20)
        self.assertEqual(image.stat().st_size, fitted)
        check(image, fitted // BLOCK)

    def test_fits_on_both_sides_of_every_queue_limit_step(self):
        for size, targets in FITS.items():
            source = self.dir / f'{size}.img'
            blank(source, size)
            for blocks in targets:
                with self.subTest(size=size, blocks=blocks):
                    image = self.dir / f'{size}-{blocks}.img'
                    host.run('cp', '-c', source, image)
                    host.run(APFS_TOOL, str(image), '0', str(blocks))
                    os.truncate(image, blocks * BLOCK)
                    check(image, blocks)
                    image.unlink()
            source.unlink()


if __name__ == '__main__':
    APFS_TOOL = os.path.abspath(sys.argv.pop(1))
    unittest.main()
