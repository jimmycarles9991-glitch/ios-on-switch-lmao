"""The inodes of one APFS volume in an image that nothing has attached: read, and their owners rewritten in place.

macOS gives root's owners to nobody else, so the image builder copies files as the user and gives them their owners
here instead (owners.py). Only fixed fields of an inode's value change (owner, group, mode, BSD flags): no record
moves, no node splits, nothing is allocated, and each node written gets its Fletcher-64 checksum again. A sealed
volume hashes its tree, so it is read but never written; a cryptex gets its owners before apfs_prepare_cryptex seals
it.

Layouts: Apple File System Reference (apfs_superblock_t, omap_phys_t, btree_node_phys_t, j_key_t, j_inode_val_t).
"""

import os
import struct
import uuid
from collections.abc import Iterator
from dataclasses import dataclass

from tools.install import apfs

OBJECT_TYPE_FSTREE = 0xE
OBJ_PHYSICAL = 0x40000000
BTNODE_HASHED, BTNODE_NOHEADER = 0x8, 0x10
BTOFF_INVALID = 0xFFFF
BTREE_INFO_BYTES = 40
APFS_TYPE_INODE = 3
OBJ_ID_MASK = (1 << 60) - 1
INCOMPAT_SEALED_VOLUME = 0x20
SB_INCOMPAT, SB_OMAP, SB_ROOT_TREE_TYPE, SB_ROOT_TREE = 56, 128, 116, 136
INODE_FLAGS, INODE_OWNER, INODE_GROUP, INODE_MODE, INODE_BYTES = 68, 72, 76, 80, 92
INO_EXT_TYPE_NAME = 4
PERMISSIONS = 0o7777
MAX_DEPTH = 16


@dataclass(frozen=True)
class Inode:
    """An inode's owner, group, mode and BSD flags; read ones also carry their parent's inode and their name."""

    uid: int
    gid: int
    mode: int
    flags: int
    parent: int = 0
    name: str = ''


@dataclass(frozen=True)
class _Record:
    paddr: int
    at: int


class Volume:
    """One volume of the container on `fd` (at byte `base`): the only one, or the one whose UUID is `volume_uuid`."""

    def __init__(self, fd: int, base: int = 0, volume_uuid: uuid.UUID | None = None) -> None:
        self.container = apfs.Container(fd, base)
        newest = self.container.superblocks()[0]
        located = self.container.omap_latest(struct.unpack_from('<Q', newest, 160)[0], apfs.object_header(newest)[1])
        found = []
        for fs_oid in apfs.Container.fs_oids(newest):
            block = self.container.read(located[fs_oid]) if fs_oid in located else b''
            if block[32:36] == apfs.APFS_MAGIC and apfs.checksum_ok(block):
                found.append(block)
        if volume_uuid is not None:
            found = [
                b for b in found if uuid.UUID(bytes=b[apfs.VOL_UUID_OFFSET : apfs.VOL_UUID_OFFSET + 16]) == volume_uuid
            ]
        if len(found) != 1:
            raise apfs.ApfsError(f'expected one volume {volume_uuid or ""}, found {len(found)}')
        sb = found[0]
        self.sealed = bool(struct.unpack_from('<Q', sb, SB_INCOMPAT)[0] & INCOMPAT_SEALED_VOLUME)
        self.physical = bool(struct.unpack_from('<I', sb, SB_ROOT_TREE_TYPE)[0] & OBJ_PHYSICAL)
        self.root = struct.unpack_from('<Q', sb, SB_ROOT_TREE)[0]
        omap = struct.unpack_from('<Q', sb, SB_OMAP)[0]
        self.objects = {} if self.physical else self.container.omap_latest(omap, apfs.object_header(sb)[1])

    def _node(self, oid: int, physical: bool) -> tuple[int, bytes]:
        paddr = oid if physical else self.objects.get(oid)
        if paddr is None:
            raise apfs.ApfsError(f'file-system tree node {oid} is not in the volume object map')
        node = self.container.read(paddr)
        # A sealed volume stores its tree without object headers: the seal's hashes stand in for their checksums.
        if struct.unpack_from('<H', node, 32)[0] & BTNODE_NOHEADER:
            return paddr, node
        kind = apfs.object_header(node)
        if not apfs.checksum_ok(node) or kind[2] not in (apfs.OBJECT_TYPE_BTREE, apfs.OBJECT_TYPE_BTREE_NODE):
            raise apfs.ApfsError(f'bad file-system tree node at block {paddr}')
        if kind[3] != OBJECT_TYPE_FSTREE:
            raise apfs.ApfsError(f'block {paddr} is a tree node of subtype {kind[3]:#x}, not the file-system tree')
        return paddr, node

    def _entries(self, node: bytes) -> Iterator[tuple[int, int, int, int]]:
        """(key offset, key length, value offset, value length) of each record, offsets from the block's start."""
        flags, _level, count = struct.unpack_from('<HHI', node, 32)
        table_off, table_len = struct.unpack_from('<HH', node, 40)
        if flags & apfs.BTNODE_FIXED_KV_SIZE:
            raise apfs.ApfsError('a file-system tree node with fixed-size records')
        toc = 56 + table_off
        keys = toc + table_len
        values_end = len(node) - (BTREE_INFO_BYTES if flags & apfs.BTNODE_ROOT else 0)
        for i in range(count):
            k_off, k_len, v_off, v_len = struct.unpack_from('<HHHH', node, toc + 8 * i)
            if v_off != BTOFF_INVALID:
                yield keys + k_off, k_len, values_end - v_off, v_len

    def _nodes(self) -> Iterator[tuple[int, bytes]]:
        root = self._node(self.root, self.physical)
        # A hashed (sealed) tree names each child by its oid less the root's, beside the hash that binds it.
        base = self.root if struct.unpack_from('<H', root[1], 32)[0] & BTNODE_HASHED else 0
        pending = [(root, 0)]
        while pending:
            (paddr, node), depth = pending.pop()
            if depth > MAX_DEPTH:
                raise apfs.ApfsError('file-system tree too deep')
            yield paddr, node
            if struct.unpack_from('<H', node, 34)[0]:
                children = [struct.unpack_from('<Q', node, v)[0] for _, _, v, _ in self._entries(node)]
                pending += [(self._node(base + child, self.physical), depth + 1) for child in children]

    def _leaves(self) -> Iterator[tuple[int, bytes]]:
        return ((paddr, node) for paddr, node in self._nodes() if struct.unpack_from('<H', node, 34)[0] == 0)

    def tree_bytes(self) -> int:
        """Return what the volume's file-system tree takes: its nodes, in blocks."""
        return sum(1 for _ in self._nodes()) * self.container.block_size

    def _inode_records(self) -> Iterator[tuple[int, int, bytes, int, int]]:
        """(inode number, block, node, value offset, value length) of every inode record."""
        for paddr, node in self._leaves():
            for k, _, v, v_len in self._entries(node):
                key = struct.unpack_from('<Q', node, k)[0]
                if key >> 60 == APFS_TYPE_INODE:
                    if v_len < INODE_BYTES:
                        raise apfs.ApfsError(f'inode {key & OBJ_ID_MASK} has a {v_len}-byte value')
                    yield key & OBJ_ID_MASK, paddr, node, v, v_len

    def inodes(self) -> dict[int, Inode]:
        return {ino: _decode(node, v, v_len) for ino, _, node, v, v_len in self._inode_records()}

    def rewrite(self, wanted: dict[int, Inode]) -> int:
        """Give each inode in `wanted` its owner, group, permission bits and flags; return how many changed.

        The type bits of the mode stay the inode's. Every inode named must exist.
        """
        if self.sealed:
            raise apfs.ApfsError('the volume is sealed: its owners can no longer change')
        changed: dict[int, bytearray] = {}
        seen = set()
        for ino, paddr, node, v, _ in self._inode_records():
            if ino not in wanted:
                continue
            seen.add(ino)
            block = changed.setdefault(paddr, bytearray(node))
            want = wanted[ino]
            mode = (struct.unpack_from('<H', block, v + INODE_MODE)[0] & ~PERMISSIONS) | (want.mode & PERMISSIONS)
            struct.pack_into('<III', block, v + INODE_FLAGS, want.flags, want.uid, want.gid)
            struct.pack_into('<H', block, v + INODE_MODE, mode)
        missing = sorted(set(wanted) - seen)
        if missing:
            raise apfs.ApfsError(f'no inode {missing[0]} in the volume ({len(missing)} missing)')
        for paddr, block in changed.items():
            self.container.write(paddr, apfs.with_checksum(bytes(block)))
        os.fsync(self.container.fd)
        return len(seen)


def _decode(node: bytes, v: int, v_len: int) -> Inode:
    flags, uid, gid = struct.unpack_from('<III', node, v + INODE_FLAGS)
    mode = struct.unpack_from('<H', node, v + INODE_MODE)[0]
    return Inode(uid, gid, mode, flags, struct.unpack_from('<Q', node, v)[0], _name(node[v + INODE_BYTES : v + v_len]))


def _name(xfields: bytes) -> str:
    """Return an inode's INO_EXT_TYPE_NAME field (xf_blob_t: x_field_t headers, then 8-byte-aligned values)."""
    if len(xfields) < 4:
        return ''
    count = struct.unpack_from('<H', xfields, 0)[0]
    at = 4 + 4 * count
    for i in range(count):
        kind, _flags, size = struct.unpack_from('<BBH', xfields, 4 + 4 * i)
        if kind == INO_EXT_TYPE_NAME:
            return xfields[at : at + size].split(b'\0', 1)[0].decode('utf-8', 'replace')
        at += -(-size // 8) * 8
    return ''
