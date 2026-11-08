"""The extent plan, applied to a partition full of someone else's bytes, gives back the image wherever APFS reads."""

import os
import struct
import tempfile
import unittest
import uuid

from tools.install import extents, manifest

BLOCK = 4096
SIZE = 64 << 20


def nxsb(desc_base: int, desc_blocks: int, data_base: int, data_blocks: int):
    block = bytearray(BLOCK)
    block[32:36] = b'NXSB'
    struct.pack_into('<IQ', block, 36, BLOCK, SIZE // BLOCK)
    block[72:88] = bytes(range(16))
    struct.pack_into('<IIQQ', block, 104, desc_blocks, data_blocks, desc_base, data_base)
    return bytes(block)


def apply(plan: list[tuple[int, int, int]], image_fd: int, target: bytes):
    """Do what install-ios.bin does: copy extents from the concatenated sources, zero extents as zeros."""
    out = bytearray(target)
    for at, source, n in plan:
        out[at : at + n] = bytes(n) if source == manifest.ZERO else os.pread(image_fd, n, at)
    return bytes(out)


class ExtentsTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.mkdtemp()
        self.path = os.path.join(directory, 'partition.img')
        self.fd = os.open(self.path, os.O_RDWR | os.O_CREAT)
        self.addCleanup(os.close, self.fd)
        os.ftruncate(self.fd, SIZE)

    def write(self, at: int, data: bytes):
        os.pwrite(self.fd, data, at)

    def test_container_reads_the_checkpoint_descriptor_area(self):
        block, count, container, areas = extents.container(nxsb(1, 8, 9, 32))
        self.assertEqual((block, count, container), (BLOCK, SIZE // BLOCK, uuid.UUID(bytes=bytes(range(16)))))
        self.assertEqual(areas, [(BLOCK, 9 * BLOCK)])
        with self.assertRaises(extents.ExtentError):
            extents.container(nxsb(1, 8 | extents.NX_NONCONTIGUOUS, 9, 32))
        with self.assertRaises(extents.ExtentError):
            extents.container(bytes(BLOCK))

    def test_plan_rebuilds_data_and_zeroes_checkpoints(self):
        self.write(0, nxsb(1, 8, 9, 32))
        self.write(3 * BLOCK, b'descriptor')
        self.write(5 << 20, os.urandom(3 * BLOCK + 17))
        self.write(40 << 20, os.urandom(1 << 20))
        data = extents.data_ranges(self.fd, 0, SIZE, BLOCK)
        _, _, _, areas = extents.container(os.pread(self.fd, BLOCK, 0))
        plan = extents.plan(data, areas, SIZE, BLOCK)
        self.assertEqual(plan[0][:2], (0, 0))
        stale = os.urandom(SIZE)
        rebuilt = apply(plan, self.fd, stale)
        image = os.pread(self.fd, SIZE, 0)
        for start, end in data + areas:
            self.assertEqual(rebuilt[start:end], image[start:end])
        written = sum(n for _, _, n in plan)
        self.assertLess(written, 4 << 20)
        source = 0
        for _at, src, n in plan:
            if src != manifest.ZERO:
                self.assertEqual(src, source)
                source += n

    def test_plan_joins_extents_to_fit_the_limit(self):
        self.write(0, nxsb(1, 1, 2, 1))
        for i in range(1, 64):
            self.write(i * (1 << 20), b'x')
        data = extents.data_ranges(self.fd, 0, SIZE, BLOCK)
        plan = extents.plan(data, [], SIZE, BLOCK, limit=8)
        self.assertLessEqual(len(plan), 8)
        rebuilt = apply(plan, self.fd, bytes(SIZE))
        self.assertEqual(rebuilt, os.pread(self.fd, SIZE, 0))

    def test_plan_refuses_an_image_without_a_first_block(self):
        self.write(1 << 20, b'x')
        with self.assertRaises(extents.ExtentError):
            extents.plan(extents.data_ranges(self.fd, 0, SIZE, BLOCK), [], SIZE, BLOCK)

    def test_plan_round_trips_through_the_manifest(self):
        self.write(0, nxsb(1, 8, 9, 32))
        self.write(7 << 20, b'y' * BLOCK)
        plan = extents.plan(extents.data_ranges(self.fd, 0, SIZE, BLOCK), [(BLOCK, 41 * BLOCK)], SIZE, BLOCK)
        source = sum(n for _, s, n in plan if s != manifest.ZERO)
        m = manifest.Manifest(
            SIZE,
            SIZE,
            BLOCK,
            extents.container(os.pread(self.fd, BLOCK, 0))[2],
            'test',
            [manifest.Part(f'image/{i}', n) for i, n in enumerate(manifest.split(manifest.BODY_OFFSET + source))],
            plan,
            [bytes(32)] * manifest.chunks(source),
            [manifest.Boot('bl33.bin', manifest.BOOT_OFFSET, 1, [bytes(32)])],
        )
        self.assertEqual(manifest.decode(manifest.encode(m)).extents, plan)


class DataAreaTest(unittest.TestCase):
    def test_the_data_area_and_the_pool_keep_their_holes(self):
        """Only the descriptor area is zero-filled: the data area's and the pool's unwritten tails stay unwritten."""
        fd = os.open(tempfile.mktemp(), os.O_RDWR | os.O_CREAT)
        self.addCleanup(os.close, fd)
        os.ftruncate(fd, SIZE)
        os.pwrite(fd, nxsb(1, 8, 9, 32), 0)
        os.pwrite(fd, b'checkpoint map', 9 * BLOCK)
        os.pwrite(fd, b'pool bitmap', 60 * BLOCK)
        data = extents.data_ranges(fd, 0, SIZE, BLOCK)
        _, _, _, areas = extents.container(os.pread(fd, BLOCK, 0))
        plan = extents.plan(data, areas, SIZE, BLOCK)
        zeros = [(at, n) for at, src, n in plan if src == manifest.ZERO]
        self.assertEqual(zeros, [(BLOCK, 8 * BLOCK)])
        self.assertEqual(sum(n for _, src, n in plan if src != manifest.ZERO), 3 * BLOCK)


if __name__ == '__main__':
    unittest.main()
