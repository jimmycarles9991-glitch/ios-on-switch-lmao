#!/usr/bin/env python3
"""A pack record past 32 set-0 entries and a program of uniform buffers past set-0 byte 496 (pack v27).

The record's entries past 32, and their bound and memory bits, go in its far block in the blob section after its restore
pairs (raw/runtime/raw_set_entries.h); its entry count's high byte, the far block's 24-bit offset and an argument
table's entry past 254 go in bytes every record of 32 or fewer leaves zero, so such a record's bytes and blob are
version 26's.
A program's far table of uniform buffer offsets goes in the blob section too, named by its PackProgram.farUbos.
"""

import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'raw', 'pack'))
import directlink
import packfile

HEAD_ENTRIES_HIGH = 31  # PackPipeline.entriesHigh


def buffers(count: int) -> list[dict]:
    return [
        {'off': 16 * k, 'kind': 'ubo', 'stage': 'v' if k % 2 else 'f', 'slot': k // 2, 'ssbo': True, 'window': 0}
        for k in range(count)
    ]


def record(count: int, **fields: object) -> dict:
    return dict(
        key='samples=1 c0=70:15:0',
        vs=0,
        fs=1,
        blob=[],
        entries=buffers(count),
        push=[],
        vbs=[],
        set_size=16 * count,
        fetch=False,
        **fields,
    )


def read_back(data: bytes, far: list[int], spill: list[int] | None = None) -> dict:
    programs = [{'image': bytes([i]), 'data': b'', 'cbufs': [], 'far_ubos': []} for i in range(2)]
    ids = {directlink.program_id(p): i for i, p in enumerate(programs)}
    probe = {'index': 0, 'form': '', 'targets': 1, 'records': [{'regs': {}}]}
    rec = {'pipeline': data, 'key': 'samples=1 c0=70:15:0', 'programs': programs, 'samplers': [], 'probe': probe}
    return directlink.pipeline_of(dict(rec, far=far, spill=spill or []), [], ids, [])


class FarTest(unittest.TestCase):
    def test_a_record_past_32_entries_keeps_the_rest_and_their_bits_in_its_far_block(self):
        bound, memory = 1 << 3 | 1 << 35 | 1 << 39, 1 << 0 | 1 << 32 | 1 << 38
        rec = record(40, bound_entries=bound, memory_entries=memory, restore=[(0x1234, 5)])
        pipes, blob, _, _ = packfile.pipeline_sections([rec])
        restore_at = len(blob) - len(packfile.far_words(rec)) - 2
        far_at = restore_at + 2
        self.assertEqual(blob[restore_at:far_at], [0x1234, 5])
        far = blob[far_at:]
        self.assertEqual(len(far), 2 * 8 + 2)
        self.assertEqual(far[16:], [1 << 3 | 1 << 7, 1 << 0 | 1 << 6])
        self.assertEqual(pipes[28], 40)
        self.assertEqual(pipes[HEAD_ENTRIES_HIGH], 0)
        at = directlink.V20_AT + packfile.V20_FAR_AT
        self.assertEqual(int.from_bytes(pipes[at : at + 3], 'little'), far_at)
        back = read_back(bytes(pipes), far)
        self.assertEqual(back['entries'], [dict(e, residual=None) for e in rec['entries']])
        self.assertEqual((back['bound_entries'], back['memory_entries']), (bound, memory))
        self.assertEqual(packfile.far_words(back), far)

    def test_every_argument_of_a_render_pipeline_fits_and_its_count_takes_the_high_byte(self):
        rec = record(packfile.SET_ENTRIES_MAX)
        data = packfile.pipeline_bytes(dict(rec, far_at=9), 0, 0)
        self.assertEqual(len(data), directlink.PIPELINE_BYTES)
        self.assertEqual(data[28] | data[HEAD_ENTRIES_HIGH] << 8, packfile.SET_ENTRIES_MAX)
        back = read_back(data, packfile.far_words(rec))
        self.assertEqual(len(back['entries']), packfile.SET_ENTRIES_MAX)
        self.assertEqual(back['entries'][-1]['off'], 16 * (packfile.SET_ENTRIES_MAX - 1))
        with self.assertRaisesRegex(ValueError, 'past the 24 bits'):
            packfile.pipeline_bytes(dict(rec, far_at=packfile.FAR_AT_LIMIT), 0, 0)

    def test_an_argument_table_past_entry_254_round_trips_its_high_byte(self):
        entries = [*buffers(300), {'off': 4800, 'kind': 'spill_table', 'stage': 2}]
        rec = dict(record(0), entries=entries, spill_entry=[0, 301], spill=[[], [1, 2]], spill_at=4)
        data = packfile.pipeline_bytes(dict(rec, far_at=6), 0, 0)
        self.assertEqual(packfile.PIPELINE_V20.unpack_from(data, directlink.V20_AT)[24:26], (0, 301 & 0xFF))
        back = read_back(data, packfile.far_words(rec), [1, 2])
        self.assertEqual(back['spill_entry'], [0, 301])

    def test_a_record_of_32_entries_or_fewer_is_version_26s_bytes(self):
        rec = record(32, bound_entries=0xFFFF, restore=[(0x1234, 5)])
        pipes, blob, _, _ = packfile.pipeline_sections([rec])
        self.assertEqual(blob, [0x1234, 5])
        self.assertEqual(pipes[HEAD_ENTRIES_HIGH], 0)
        at = directlink.V20_AT + packfile.V20_FAR_AT
        self.assertEqual(pipes[at : at + 3], bytes(3))
        at = directlink.V22_AT + packfile.V22_SPILL_HIGH
        self.assertEqual(pipes[at : at + 2], bytes(2))

    def test_a_programs_uniform_buffers_past_byte_496_are_its_far_table_in_the_blob(self):
        near = {
            'cbufs': [1, 5 | 2 << 3],
            'image': bytes(80),
            'data': b'',
            'heap_offset': 0,
            'program': 0x30,
            'bytes': 256,
            'data_offset': 0,
            'data_bytes': 0,
        }
        far = dict(near, cbufs=[1, 5, 5 | 4 << 3], far_ubos=[0, 640 // 16, 0])
        pack = packfile.Pack([], [near, far], [], {}, [], bytearray(), [])
        data, _ = packfile.pack_bytes(pack)
        header = struct.unpack_from(f'<{2 * packfile.SECTIONS}Q', data, 24)
        programs, blob_at, blob_bytes = header[0], header[2 * 5], header[2 * 5 + 1]
        size = 44
        self.assertEqual(data[programs + 41 : programs + 44], bytes(3))
        table_at = int.from_bytes(data[programs + size + 41 : programs + size + 44], 'little') - 1
        self.assertGreaterEqual(table_at, 0)
        self.assertLessEqual(4 * (table_at + 9), blob_bytes)
        table = struct.unpack_from('<18H', data, blob_at + 4 * table_at)
        self.assertEqual(table[:3], (0, 40, 0))
        self.assertEqual(directlink.program_id(far), directlink.program_id(far))
        self.assertNotEqual(directlink.program_id(far), directlink.program_id(dict(far, far_ubos=[0, 41, 0])))
        self.assertEqual(directlink.program_id(near), directlink.program_id(dict(near, far_ubos=[])))


if __name__ == '__main__':
    unittest.main()
