#!/usr/bin/env python3
"""The built pack's one-target source-over states push the ROP's zero-source kill (blend_kill.h, pack/segments.py).

What ships is each tier-0 state's pre-built segment: its SET_ITERATED_BLEND_OPTIMIZATION word is the kill the GPU gets,
whatever the runtime's rule says (pipelines.cpp takes the segment's word). A record's blob holds only its own targets'
SET_CT_WRITE (mkpack.foreign); another target's comes from the baseline, which keeps a target's register there when
every state that draws that target writes it alike, so the generator must count only the key's targets.

usage: blend_kill_pack_test.py <pack.bin>   (//gpu/seed:pack's, of the configured IPSW kind)
"""

import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE, os.path.join(HERE, '..', 'pack')]
import directlink
import make_pack_targets_test as targets
import packfile
import segments

SET_BLEND, PER_TARGET, ALPHA_TO_COVERAGE = 0x1360, 0x1E04, 0x153C
ITERATED_BLEND_OPTIMIZATION, NOOP_SOURCE_RGBA_0000 = 0x02F4, 1
SOURCE_OVER = (0x8006, 0x4001, 0x4303)  # ADD, ONE, ONE_MINUS_SRC_ALPHA
PIPELINES, HEAP, SEGMENTS = 1, 7, 16  # runtime/pack.h PackSection
STORAGE_METADATA = 8  # runtime/pack.h PipelineStorageMetadata
STORAGE_MASK_AT = directlink.PIPELINE.size + packfile.PACK_ENTRIES * 8 + 4 + 32 * 4 + 4  # PackPipeline::storageMask
PACK = ''


def last_written(words: tuple[int, ...], method: int) -> int | None:
    """Give the value the last write of 3D `method` in a push stream leaves it (push.h lastWritten)."""
    value, i = None, 0
    while i < len(words):
        hdr = words[i]
        op, arg, subc, at = hdr >> 29, hdr >> 16 & 0x1FFF, hdr >> 13 & 7, (hdr & 0xFFF) << 2
        if op == 4:
            if subc == 0 and at == method:
                value = arg
            i += 1
            continue
        if op not in (1, 3, 5):
            i += 1
            continue
        for k in range(arg):
            to = at + 4 * k if op == 1 else (at + 4 if op == 5 and k else at)
            if subc == 0 and to == method and i + 1 + k < len(words):
                value = words[i + 1 + k]
        i += 1 + arg
    return value


def pushed_kills(path: str) -> tuple[list[int | None], list[tuple[int, int]]]:
    """Give each tier-0 state's pushed kill word (None: no segment), and each record's flags and storage mask."""
    with open(path, 'rb') as f:
        data = f.read()
    sections = [struct.unpack_from('<QQ', data, 24 + 16 * i) for i in range(packfile.SECTIONS)]
    count = sections[PIPELINES][1] // directlink.PIPELINE_BYTES
    storage = []
    for i in range(count):
        at = sections[PIPELINES][0] + i * directlink.PIPELINE_BYTES
        flags = directlink.PIPELINE.unpack_from(data, at)[-1]
        storage.append((flags, data[at + STORAGE_MASK_AT]))
    seg_at, seg_bytes = sections[SEGMENTS]
    kills: list[int | None] = []
    for heap_offset, words in segment_entries(data[seg_at : seg_at + seg_bytes]):
        if not words:
            kills.append(None)
            continue
        stream = struct.unpack_from(f'<{words}I', data, sections[HEAP][0] + heap_offset)
        kills.append(last_written(stream, ITERATED_BLEND_OPTIMIZATION))
    return kills, storage


def segment_entries(table: bytes) -> list[tuple[int, int]]:
    """Give each pipeline's (heapOffset, words) of a PackSegments section, in the layout segments.py writes it."""
    header, entry = segments._SEGMENT_TABLE, segments._SEGMENT_ENTRY
    if len(table) < header.size:
        return []
    pipelines = header.unpack_from(table, 0)[0]
    return [entry.unpack_from(table, header.size + entry.size * i)[:2] for i in range(pipelines)]


def source_over_on_target_0_alone(record: targets.Record, flags: int, storage_mask: int) -> bool:
    """Whether a record draws target 0 alone, blending source-over on colour and alpha, with neither alpha to coverage
    nor a storage attachment 0 (blend_kill.h's other refusals)"""
    regs = record.regs
    blend = tuple(regs.get(PER_TARGET + 4 * i) for i in range(6))
    return (
        targets.drawn_targets(record) == 1
        and regs.get(SET_BLEND) == 1
        and blend == SOURCE_OVER * 2
        and not regs.get(ALPHA_TO_COVERAGE, 0) & 0x11
        and not (flags & STORAGE_METADATA and storage_mask & 1)
    )


class BlendKillPackTest(unittest.TestCase):
    def test_the_segment_table_reads_as_segments_py_writes_it(self) -> None:
        table = segments.encode_table([(0x40, 12, 1), (0, 0, 0), (0x400, 3, 0)], [~0 & 0xFFFFFFFF] * 3, [(0x80, 5, 0)])
        self.assertEqual(segment_entries(table), [(0x40, 12), (0, 0), (0x400, 3)])

    def test_every_one_target_source_over_segment_pushes_the_kill(self) -> None:
        records, baseline = targets.load(PACK)
        kills, storage = pushed_kills(PACK)
        self.assertGreater(len(kills), 0, 'the pack has no pre-built segments')
        over = [
            i
            for i in range(len(kills))
            if kills[i] is not None and source_over_on_target_0_alone(records[i], *storage[i])
        ]
        self.assertGreater(len(over), 0, 'no one-target source-over state with a segment')
        unkilled = [records[i].key[:100] for i in over if kills[i] != NOOP_SOURCE_RGBA_0000]
        base_writes = [f'{baseline.get(0x1A00 + 4 * j, 0):#x}' for j in range(8)]
        self.assertEqual(
            unkilled,
            [],
            f'{len(unkilled)} of {len(over)} one-target source-over segments push no kill; the baseline SET_CT_WRITE '
            f'of targets 0-7: {base_writes}',
        )
        one_target = [i for i in range(len(records)) if source_over_on_target_0_alone(records[i], 0, 0)]
        unsegmented = sum(1 for i in one_target if i >= len(kills) or kills[i] is None)
        excluded = sum(1 for i in one_target if i not in over) - unsegmented
        print(
            f'{len(one_target)} one-target source-over records: {len(over)} with a segment, all killed; '
            f'{unsegmented} without one (tier 1: the runtime rule decides); {excluded} with a storage target 0; '
            f'{kills.count(NOOP_SOURCE_RGBA_0000)} segments killed in all'
        )


if __name__ == '__main__':
    PACK = sys.argv.pop(1)
    unittest.main()
