"""Just enough APFS to give two volumes the UUIDs the kernel was built with.

The kernel finds Data and Preboot by fixed volume UUIDs (diagnostics/switch/SwitchRawMount.h), but macOS
picks a random UUID for every new volume and has no option to choose one (newfs_apfs only has -D, a UUID derived
from the role). So after `diskutil apfs addVolume`, with the container detached, this rewrites apfs_vol_uuid in
every physical copy of the two volume superblocks the container's object maps point at (all checkpoints), each with
its Fletcher-64 checksum recomputed. Nothing else changes. The caller runs fsck_apfs afterwards.

Layouts: Apple File System Reference (nx_superblock_t, checkpoint areas, omap_phys_t, btree_node_phys_t,
apfs_superblock_t).
"""

import os
import struct
import uuid

NX_MAGIC = b'NXSB'
APFS_MAGIC = b'APSB'
OBJECT_TYPE_NX_SUPERBLOCK = 0x1
OBJECT_TYPE_BTREE = 0x2
OBJECT_TYPE_BTREE_NODE = 0x3
OBJECT_TYPE_OMAP = 0xB
OBJECT_TYPE_FS = 0xD
BTNODE_ROOT, BTNODE_LEAF, BTNODE_FIXED_KV_SIZE = 0x1, 0x2, 0x4
OMAP_VAL_DELETED = 0x1
VOL_UUID_OFFSET = 240  # apfs_superblock_t.apfs_vol_uuid
VOL_ROLE_OFFSET = 964  # apfs_superblock_t.apfs_role (u16)
VOL_NAME_OFFSET = 704  # apfs_superblock_t.apfs_volname[256]


class ApfsError(Exception):
    pass


def fletcher64(block: bytes) -> int:
    """APFS object checksum of `block` (the first 8 bytes, the stored checksum, are excluded)."""
    words = struct.unpack_from(f'<{(len(block) - 8) // 4}I', block, 8)
    sum1 = sum2 = 0
    for w in words:
        sum1 = (sum1 + w) % 0xFFFFFFFF
        sum2 = (sum2 + sum1) % 0xFFFFFFFF
    c1 = 0xFFFFFFFF - ((sum1 + sum2) % 0xFFFFFFFF)
    c2 = 0xFFFFFFFF - ((sum1 + c1) % 0xFFFFFFFF)
    return (c2 << 32) | c1


def checksum_ok(block: bytes) -> bool:
    return struct.unpack_from('<Q', block, 0)[0] == fletcher64(block)


def with_checksum(block: bytes) -> bytes:
    out = bytearray(block)
    struct.pack_into('<Q', out, 0, fletcher64(bytes(out)))
    return bytes(out)


def object_header(block: bytes) -> tuple[int, int, int, int]:
    oid, xid, kind, subtype = struct.unpack_from('<QQII', block, 8)
    return oid, xid, kind & 0xFFFF, subtype


class Container:
    """An APFS container on a file or device (fd), starting at byte `base` (the partition's start)."""

    def __init__(self, fd: int, base: int = 0):
        self.fd, self.base = fd, base
        block0 = os.pread(fd, 4096, base)
        if block0[32:36] != NX_MAGIC:
            raise ApfsError(f'no APFS container superblock at offset {base}')
        self.block_size = struct.unpack_from('<I', block0, 36)[0]
        if self.block_size != 4096:
            block0 = os.pread(fd, self.block_size, base)
        self.block0 = block0

    def read(self, paddr: int) -> bytes:
        data = os.pread(self.fd, self.block_size, self.base + paddr * self.block_size)
        if len(data) != self.block_size:
            raise ApfsError(f'short read of block {paddr}')
        return data

    def write(self, paddr: int, data: bytes) -> None:
        if len(data) != self.block_size or os.pwrite(self.fd, data, self.base + paddr * self.block_size) != len(data):
            raise ApfsError(f'write of block {paddr} failed')

    def superblocks(self) -> list[bytes]:
        """Every valid container superblock in the checkpoint descriptor area, newest first, plus block 0."""
        desc_blocks, _ = struct.unpack_from('<II', self.block0, 104)
        desc_base = struct.unpack_from('<Q', self.block0, 112)[0]
        if desc_blocks & 0x80000000:
            raise ApfsError('non-contiguous checkpoint descriptor area is not supported')
        found = {}
        for i in range(desc_blocks):
            block = self.read(desc_base + i)
            if block[32:36] == NX_MAGIC and checksum_ok(block) and object_header(block)[2] == OBJECT_TYPE_NX_SUPERBLOCK:
                found[object_header(block)[1]] = block
        if checksum_ok(self.block0):
            found.setdefault(object_header(self.block0)[1], self.block0)
        if not found:
            raise ApfsError('no valid container superblock')
        return [found[xid] for xid in sorted(found, reverse=True)]

    @staticmethod
    def uuid_of(nxsb: bytes) -> uuid.UUID:
        return uuid.UUID(bytes=nxsb[72:88])

    @staticmethod
    def fs_oids(nxsb: bytes) -> list[int]:
        max_fs = struct.unpack_from('<I', nxsb, 180)[0]
        return [oid for oid in struct.unpack_from(f'<{min(max_fs, 100)}Q', nxsb, 184) if oid]

    def omap_entries(self, omap_paddr: int) -> dict[tuple[int, int], int]:
        """{(oid, xid): paddr} of the object map at physical address omap_paddr."""
        omap = self.read(omap_paddr)
        if object_header(omap)[2] != OBJECT_TYPE_OMAP or not checksum_ok(omap):
            raise ApfsError(f'bad object map at block {omap_paddr}')
        tree = struct.unpack_from('<Q', omap, 48)[0]
        entries = {}
        self._walk(tree, entries, depth=0)
        return entries

    def omap_latest(self, omap_paddr: int, xid: int) -> dict[int, int]:
        """{oid: paddr} of the object map at omap_paddr as of transaction `xid`: each oid's newest live mapping."""
        newest: dict[int, tuple[int, int, int]] = {}
        for (oid, at), (flags, paddr) in self._entries_with_flags(omap_paddr).items():
            if at <= xid and (oid not in newest or at > newest[oid][0]):
                newest[oid] = (at, flags, paddr)
        return {oid: paddr for oid, (_, flags, paddr) in newest.items() if not flags & OMAP_VAL_DELETED}

    def _entries_with_flags(self, omap_paddr: int) -> dict[tuple[int, int], tuple[int, int]]:
        omap = self.read(omap_paddr)
        if object_header(omap)[2] != OBJECT_TYPE_OMAP or not checksum_ok(omap):
            raise ApfsError(f'bad object map at block {omap_paddr}')
        entries = {}
        self._walk(struct.unpack_from('<Q', omap, 48)[0], entries, depth=0, flags=True)
        return entries

    def _walk(self, paddr: int, entries: dict, depth: int, flags: bool = False) -> None:
        if depth > 16:
            raise ApfsError('object map tree too deep')
        node = self.read(paddr)
        if not checksum_ok(node) or object_header(node)[2] not in (OBJECT_TYPE_BTREE, OBJECT_TYPE_BTREE_NODE):
            raise ApfsError(f'bad object map node at block {paddr}')
        node_flags, _level, nkeys = struct.unpack_from('<HHI', node, 32)
        table_off, table_len = struct.unpack_from('<HH', node, 40)
        if not node_flags & BTNODE_FIXED_KV_SIZE:
            raise ApfsError(f'object map node {paddr} is not fixed-size')
        toc = 56 + table_off
        keys = toc + table_len
        value_end = self.block_size - (40 if node_flags & BTNODE_ROOT else 0)
        for i in range(nkeys):
            k, v = struct.unpack_from('<HH', node, toc + 4 * i)
            oid, xid = struct.unpack_from('<QQ', node, keys + k)
            if node_flags & BTNODE_LEAF:
                value_flags, _size, target = struct.unpack_from('<IIQ', node, value_end - v)
                entries[(oid, xid)] = (value_flags, target) if flags else target
            else:
                child = struct.unpack_from('<Q', node, value_end - v)[0]
                self._walk(child, entries, depth + 1, flags)

    def volume_blocks(self) -> dict[int, bytes]:
        """{paddr: block} of every volume superblock any checkpoint's object map points at."""
        blocks = {}
        for nxsb in self.superblocks():
            omap = struct.unpack_from('<Q', nxsb, 160)[0]
            entries = self.omap_entries(omap)
            for fs_oid in self.fs_oids(nxsb):
                for (oid, _xid), paddr in entries.items():
                    if oid == fs_oid and paddr not in blocks:
                        block = self.read(paddr)
                        if block[32:36] == APFS_MAGIC and object_header(block)[2] == OBJECT_TYPE_FS:
                            blocks[paddr] = block
        return blocks

    def volumes(self) -> list[tuple[uuid.UUID, int, str]]:
        """[(uuid, role, name)] of the newest checkpoint's volumes."""
        newest = self.superblocks()[0]
        entries = self.omap_entries(struct.unpack_from('<Q', newest, 160)[0])
        out = []
        for fs_oid in self.fs_oids(newest):
            versions = sorted((xid, paddr) for (oid, xid), paddr in entries.items() if oid == fs_oid)
            if not versions:
                continue
            block = self.read(versions[-1][1])
            out.append(
                (
                    uuid.UUID(bytes=block[VOL_UUID_OFFSET : VOL_UUID_OFFSET + 16]),
                    struct.unpack_from('<H', block, VOL_ROLE_OFFSET)[0],
                    block[VOL_NAME_OFFSET : VOL_NAME_OFFSET + 256].split(b'\0', 1)[0].decode('utf-8', 'replace'),
                )
            )
        return out

    def set_volume_uuids(
        self, mapping: dict[uuid.UUID, uuid.UUID], dry_run: bool = False
    ) -> list[tuple[int, uuid.UUID, uuid.UUID]]:
        """Rewrite every superblock copy of the volumes in `mapping` {old uuid: new uuid}; return the patched paddrs."""
        patched = []
        new_values = set(mapping.values())
        for paddr, block in sorted(self.volume_blocks().items()):
            current = uuid.UUID(bytes=block[VOL_UUID_OFFSET : VOL_UUID_OFFSET + 16])
            if current in new_values and current not in mapping:
                continue
            if current not in mapping:
                continue
            if not checksum_ok(block):
                raise ApfsError(f'volume superblock at block {paddr} has a bad checksum')
            out = bytearray(block)
            out[VOL_UUID_OFFSET : VOL_UUID_OFFSET + 16] = mapping[current].bytes
            out = with_checksum(bytes(out))
            if not dry_run:
                self.write(paddr, out)
            patched.append((paddr, current, mapping[current]))
        if not dry_run:
            os.fsync(self.fd)
        return patched
