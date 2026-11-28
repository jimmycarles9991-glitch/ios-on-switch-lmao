"""Fletcher-64 and the volume-UUID rewrite on a synthetic container (block 0, checkpoint area, omap, two volumes)."""

import os
import struct
import tempfile
import unittest
import uuid

from tools.install import apfs

BS = 4096


def obj(oid: int, xid: int, kind: int, body: bytes, subtype: int = 0):
    block = bytearray(BS)
    struct.pack_into('<QQII', block, 8, oid, xid, kind, subtype)
    block[32 : 32 + len(body)] = body
    return apfs.with_checksum(bytes(block))


def container(path: str, volumes: list[tuple[uuid.UUID, int]], checkpoints: int = 2):
    """Write a minimal container to `path`.

    NXSB at 0 and in a 4-block descriptor area at 1, an omap (block 10, root+leaf tree block 11), volume superblocks
    at 20+.
    """
    blocks = {}
    fs_oids = [1026 + i for i in range(len(volumes))]
    leaf = bytearray(BS)
    struct.pack_into('<QQII', leaf, 8, 11, 5, 0x40000003, 0xB)  # btree root node (physical), omap subtype
    flags = apfs.BTNODE_ROOT | apfs.BTNODE_LEAF | apfs.BTNODE_FIXED_KV_SIZE
    struct.pack_into('<HHI', leaf, 32, flags, 0, len(volumes))
    struct.pack_into('<HH', leaf, 40, 0, 4 * len(volumes))  # table space: toc
    keys, value_end = 56 + 4 * len(volumes), BS - 40
    for i, (vol_uuid, role) in enumerate(volumes):
        struct.pack_into('<HH', leaf, 56 + 4 * i, 16 * i, 16 * (i + 1))
        struct.pack_into('<QQ', leaf, keys + 16 * i, fs_oids[i], 5)
        struct.pack_into('<IIQ', leaf, value_end - 16 * (i + 1), 0, BS, 20 + i)
        sb = bytearray(BS - 32)
        sb[0:4] = apfs.APFS_MAGIC
        sb[apfs.VOL_UUID_OFFSET - 32 : apfs.VOL_UUID_OFFSET - 16] = vol_uuid.bytes
        struct.pack_into('<H', sb, apfs.VOL_ROLE_OFFSET - 32, role)
        sb[apfs.VOL_NAME_OFFSET - 32 : apfs.VOL_NAME_OFFSET - 32 + 4] = b'vol' + bytes([48 + i])
        blocks[20 + i] = obj(fs_oids[i], 5, apfs.OBJECT_TYPE_FS, bytes(sb))
    blocks[11] = apfs.with_checksum(bytes(leaf))
    omap = bytearray(BS - 32)
    struct.pack_into('<Q', omap, 48 - 32, 11)
    blocks[10] = obj(10, 5, apfs.OBJECT_TYPE_OMAP, bytes(omap))
    nx = bytearray(BS - 32)
    nx[0:4] = apfs.NX_MAGIC
    struct.pack_into('<IQ', nx, 4, BS, 64)
    struct.pack_into('<II', nx, 104 - 32, 4, 0)
    struct.pack_into('<Q', nx, 112 - 32, 1)
    struct.pack_into('<Q', nx, 160 - 32, 10)
    struct.pack_into('<I', nx, 180 - 32, 4)
    for i, oid in enumerate(fs_oids):
        struct.pack_into('<Q', nx, 184 - 32 + 8 * i, oid)
    for c in range(checkpoints):
        blocks[1 + c] = obj(1, 4 + c, apfs.OBJECT_TYPE_NX_SUPERBLOCK, bytes(nx))
    blocks[0] = blocks[checkpoints]
    with open(path, 'wb') as f:
        f.truncate(64 * BS)
        for n, data in blocks.items():
            f.seek(n * BS)
            f.write(data)


class ApfsTest(unittest.TestCase):
    def test_fletcher64_known_answer(self):
        block = apfs.with_checksum(bytes(BS))
        self.assertTrue(apfs.checksum_ok(block))
        self.assertFalse(apfs.checksum_ok(block[:100] + b'\1' + block[101:]))

    def test_rewrites_the_two_volume_uuids_only(self):
        system, data, preboot = uuid.uuid4(), uuid.uuid4(), uuid.uuid4()
        want_data = uuid.UUID('D1B1D053-D0A6-496C-8625-A947A3DA863E')
        want_preboot = uuid.UUID('4AA4A761-5138-439D-9A30-D4138F6887FA')
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, 'c.img')
            container(path, [(system, 1), (data, 0x40), (preboot, 0x10)])
            fd = os.open(path, os.O_RDWR)
            try:
                c = apfs.Container(fd)
                self.assertEqual(len(c.superblocks()), 2)
                self.assertEqual([(u, r) for u, r, _ in c.volumes()], [(system, 1), (data, 0x40), (preboot, 0x10)])
                patched = c.set_volume_uuids({data: want_data, preboot: want_preboot})
                self.assertEqual(len(patched), 2)
                self.assertEqual(
                    [(u, r) for u, r, _ in c.volumes()], [(system, 1), (want_data, 0x40), (want_preboot, 0x10)]
                )
                for paddr in (20, 21, 22):
                    self.assertTrue(apfs.checksum_ok(c.read(paddr)))
                # idempotent: nothing left to patch
                self.assertEqual(c.set_volume_uuids({data: want_data, preboot: want_preboot}), [])
            finally:
                os.close(fd)

    def test_sets_volume_feature_flags_idempotently(self):
        system, data, preboot = uuid.uuid4(), uuid.uuid4(), uuid.uuid4()
        want_ro = apfs.APFS_ROCOMPAT_CLONE_MAPPING | apfs.APFS_ROCOMPAT_ATTRIBUTION_TAGS
        want_incompat = apfs.APFS_INCOMPAT_EXPANDED_RECORDS
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, 'c.img')
            container(path, [(system, 1), (data, 0x40), (preboot, 0x10)])
            fd = os.open(path, os.O_RDWR)
            try:
                c = apfs.Container(fd)
                patched = c.set_volume_features({data: (want_ro, want_incompat)})
                self.assertEqual(patched, [(21, data, want_ro, want_incompat)])
                for paddr, expected in ((20, (0, 0)), (21, (want_ro, want_incompat)), (22, (0, 0))):
                    blk = c.read(paddr)
                    self.assertTrue(apfs.checksum_ok(blk))
                    self.assertEqual(struct.unpack_from('<QQ', blk, apfs.VOL_ROCOMPAT_OFFSET), expected)
                self.assertEqual(c.set_volume_features({data: (want_ro, want_incompat)}), [])
            finally:
                os.close(fd)


if __name__ == '__main__':
    unittest.main()
