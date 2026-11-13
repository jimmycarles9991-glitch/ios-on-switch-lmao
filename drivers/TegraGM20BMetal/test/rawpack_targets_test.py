#!/usr/bin/env python3
"""Each record of the built pack holds its own colour targets' registers, and none holds a target's separate alpha.

The targets a record's draw writes are its key's colours and, in its two-plane form (PipelineTwoPlane), each
RGB10A8_2P_XR10 colour's alpha target (mkpack.target_mask). Its blob holds every one of their SET_CT_WRITE (0x1a00),
SET_BLEND (0x1360) and per-target blend (0x1e00 block) registers that varies across the pack, and no other target's: a
register it leaves out is the previous draw's. Metal's blend always has separate alpha factors, so every target's
SET_BLEND_PER_TARGET_SEPARATE_FOR_ALPHA (0x1e00 + 0x20 j) is 1 under every draw: no record holds one, and target 0's is
in the baseline.

usage: rawpack_targets_test.py <pack.bin>   (//gpu/seed:raw_pack's, of the configured IPSW kind)
"""

import os
import struct
import sys
import unittest
from typing import NamedTuple

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'raw', 'pack'))
import directlink
import mkpack
import packfile

PIPELINES, BASELINE, BLOB, STRINGS = 1, 3, 5, 6  # runtime/raw_pack.h PackSection
TWO_PLANE = 128  # PipelineTwoPlane
SET_CT_WRITE, SEPARATE_FOR_ALPHA = 0x1A00, 0x1E00
PACK = ''


class Record(NamedTuple):
    key: str
    two_plane: bool
    regs: dict[int, int]


def load(path: str) -> tuple[list[Record], dict[int, int]]:
    """Give a pack's records, each with its blob's registers, and its baseline."""
    with open(path, 'rb') as f:
        data = f.read()
    version, _programs, count = struct.unpack_from('<3I', data, 8)
    if data[:8] != packfile.MAGIC or version != packfile.VERSION:
        raise ValueError(f'{path}: not a version {packfile.VERSION} pack')
    sections = [struct.unpack_from('<QQ', data, 24 + 16 * i) for i in range(packfile.SECTIONS)]
    if sections[PIPELINES][1] != count * directlink.PIPELINE_BYTES:
        raise ValueError(f'{path}: its pipeline section is not {count} records')
    strings, blob = sections[STRINGS][0], sections[BLOB][0]
    records = []
    for i in range(count):
        head = directlink.PIPELINE.unpack_from(data, sections[PIPELINES][0] + i * directlink.PIPELINE_BYTES)
        key_at, blob_at, dwords, flags = strings + head[1], blob + 4 * head[4], head[5], head[12]
        key = data[key_at : data.index(b'\0', key_at)].decode()
        words = struct.unpack_from(f'<{dwords}I', data, blob_at)
        records.append(Record(key, bool(flags & TWO_PLANE), dict(zip(words[0::2], words[1::2], strict=True))))
    at, size = sections[BASELINE]
    return records, dict(struct.iter_unpack('<II', data[at : at + size]))


def drawn_targets(record: Record) -> int:
    """Give the targets a record's draw writes, bit j target j."""
    return mkpack.target_mask(record.key, record.two_plane)


def target_of(method: int) -> int | None:
    """Give the colour target whose own register `method` is (mkpack.foreign's), None for another register."""
    for base, stride in ((0x1E00, 0x20), (0x1A00, 4), (0x1360, 4)):
        if base <= method < base + 8 * stride:
            return (method - base) // stride
    return None


def colour_writes(methods: set[int] | dict[int, int]) -> int:
    """Give the targets whose SET_CT_WRITE is among `methods`, bit j target j."""
    return sum(1 << j for j in range(8) if SET_CT_WRITE + 4 * j in methods)


def described(records: list[Record], held: str) -> str:
    """Give how many `records` there are, how many of them two-plane, and the first's key, targets and `held`."""
    if not records:
        return ''
    first = records[0]
    two_plane = sum(1 for r in records if r.two_plane)
    return (
        f'{len(records)} records ({two_plane} two-plane), the first {first.key[:120]!r} drawing targets '
        f'{drawn_targets(first):#010b} {held}: {held_by(first, held)}'
    )


def held_by(record: Record, held: str) -> str:
    """Give what `record` holds of `held`: its colour writes or its target registers."""
    if held == 'writes':
        return f'{colour_writes(record.regs):#010b}'
    return ' '.join(f'{m:x}' for m in sorted(record.regs) if target_of(m) is not None)


class TargetsTest(unittest.TestCase):
    records: list[Record]
    baseline: dict[int, int]
    varying: set[int]

    @classmethod
    def setUpClass(cls) -> None:
        cls.records, cls.baseline = load(PACK)
        cls.varying = {m for r in cls.records for m in r.regs if target_of(m) is not None}

    def test_a_records_colour_writes_are_the_targets_it_draws(self) -> None:
        varying = colour_writes(self.varying)
        wrong = [r for r in self.records if colour_writes(r.regs) != drawn_targets(r) & varying]
        self.assertEqual(len(wrong), 0, described(wrong, 'writes'))

    def test_a_record_holds_each_varying_register_of_its_targets_and_no_others(self) -> None:
        def own(r: Record) -> set[int]:
            return {m for m in self.varying if drawn_targets(r) >> target_of(m) & 1}

        wrong = [r for r in self.records if {m for m in r.regs if target_of(m) is not None} != own(r)]
        self.assertEqual(len(wrong), 0, described(wrong, 'holds'))

    def test_separate_alpha_is_no_records_and_target_0s_is_the_baselines(self) -> None:
        separate = {SEPARATE_FOR_ALPHA + 0x20 * j for j in range(8)}
        holding = [r for r in self.records if separate & r.regs.keys()]
        self.assertEqual(
            (self.baseline.get(SEPARATE_FOR_ALPHA), len(holding)),
            (1, 0),
            f'0x1e00 in the baseline, records holding a SET_BLEND_PER_TARGET_SEPARATE_FOR_ALPHA; '
            f'{described(holding, "holds")}',
        )


if __name__ == '__main__':
    PACK = sys.argv.pop(1)
    unittest.main()
