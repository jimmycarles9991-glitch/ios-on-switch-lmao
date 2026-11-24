#!/usr/bin/env python3
"""A pack record past 32 set-0 entries and a program of uniform buffers past set-0 byte 496 (pack v27).

The record's entries past 32, and their bound and memory bits, go in its far block in the blob section after its restore
pairs (set_entries.h); its entry count's high byte, the far block's 24-bit offset and an argument
table's entry past 254 go in bytes every record of 32 or fewer leaves zero, so such a record's bytes and blob are
version 26's.
A program's far table of uniform buffer offsets goes in the blob section too, named by its PackProgram.farUbos.
"""

import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'pack'))
import directlink
import packfile

HEAD_ENTRIES_HIGH = 31  # PackPipeline.entriesHigh
WRITES_MEMORY = 1  # runtime/pack.h kProgramWritesMemory


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
        size = packfile.PROGRAM_BYTES
        self.assertEqual(data[programs + 41 : programs + 44], bytes(3))
        table_at = int.from_bytes(data[programs + size + 41 : programs + size + 44], 'little') - 1
        self.assertGreaterEqual(table_at, 0)
        self.assertLessEqual(4 * (table_at + 9), blob_bytes)
        table = struct.unpack_from('<18H', data, blob_at + 4 * table_at)
        self.assertEqual(table[:3], (0, 40, 0))
        self.assertEqual(directlink.program_id(far), directlink.program_id(far))
        self.assertNotEqual(directlink.program_id(far), directlink.program_id(dict(far, far_ubos=[0, 41, 0])))
        self.assertEqual(directlink.program_id(near), directlink.program_id(dict(near, far_ubos=[])))


class FastKeysTest(unittest.TestCase):
    """The PackFastKeys section (runtime/pack.h PackFastKey): each fast key the run of its state's records.

    The runtime searches the pipelines unchecked, so a link of records out of key hash order fails.
    """

    KEYS = sorted(('samples=1 | v a | f b', 'samples=4 | v a | f b'), key=lambda k: packfile.fnv(k.encode()))

    def test_a_fast_key_names_its_states_records(self):
        first, second = (packfile.fnv(k.encode()) for k in self.KEYS)
        pipes = [dict(record(0), key=key) for key in (self.KEYS[0], self.KEYS[0], self.KEYS[1])]
        section, _ = packfile.fast_key_bytes({5: second, 3: first, 9: 1234}, pipes)
        runs = [struct.unpack_from('<QII', section, 16 * i) for i in range(len(section) // 16)]
        self.assertEqual(runs, [(3, 0, 2), (5, 2, 1)])

    def test_records_out_of_key_hash_order_fail_the_link(self):
        pipes = [dict(record(0), key=self.KEYS[1]), dict(record(0), key=self.KEYS[0])]
        with self.assertRaisesRegex(ValueError, 'key hash order'):
            packfile.pack_bytes(packfile.Pack(pipes, [], [], {}, [], bytearray(), []))


class TiersTest(unittest.TestCase):
    """Each section's tier 0 first, then tier 1, each in key order; the header's tier0 table names tier 0's part.

    Tier 0 is the launch set (pack/make_pack.py COLD_TIERS): the runtime reads its part alone for any state of it.
    """

    KEYS = FastKeysTest.KEYS

    def state(self, tier: int, image: bytes, key: str = 'samples=1 | v a | f b', twin: int = 0) -> dict:
        vertex = {'image': bytes(80) + image, 'data': b'', 'cbufs': [], 'far_ubos': []}
        fragment = {'image': bytes(80) + image + b'f', 'data': b'', 'cbufs': [], 'far_ubos': []}
        ids = [directlink.program_id(p) for p in (vertex, fragment)]
        pipeline = packfile.pipeline_bytes(dict(record(0), key=key, stride_twin=twin), 0, 0)
        probe = {'records': [{'prog': {1: (ids[0],), 5: (ids[1],)}}]}
        return {'key': key, 'tier': tier, 'pipeline': pipeline, 'programs': [vertex, fragment], 'probe': probe}

    def test_tier_zeros_programs_come_first_and_end_on_the_boundary(self):
        shared, cold, launch = self.state(1, b'shared'), self.state(1, b'cold'), self.state(0, b'launch')
        launch['programs'][0] = shared['programs'][0]
        launch['probe']['records'][0]['prog'][1] = shared['probe']['records'][0]['prog'][1]
        programs = {directlink.program_id(p): p for rec in (shared, cold, launch) for p in rec['programs']}
        (_, _, recs), _, launch_programs, launch_heap = directlink.lay_out_tiers(
            [shared, cold, launch], programs, {}, []
        )
        self.assertEqual(launch_programs, 2)
        self.assertEqual({recs[i]['id'] for i in range(2)}, {directlink.program_id(p) for p in launch['programs']})
        self.assertEqual(launch_heap % directlink.TIER_ALIGN, 0)
        last = max(recs[i]['heap_offset'] + recs[i]['bytes'] for i in range(2))
        self.assertGreaterEqual(launch_heap - last, directlink.PREFETCH)
        self.assertTrue(all(r['heap_offset'] >= launch_heap for r in recs[2:]))

    def test_a_state_takes_its_lowest_lines_tier_and_its_stride_twin_its_lowest_referrers(self):
        twin = self.state(1, b'twin', key=self.KEYS[0])
        launch = self.state(1, b'launch', key=self.KEYS[1], twin=packfile.key_hash(twin))
        lines = [(5, packfile.key_hash(launch), 1), (7, packfile.key_hash(twin), 1), (5, packfile.key_hash(launch), 0)]
        fast, tiers = directlink.fast_keys(lines)
        self.assertEqual(fast, {5: packfile.key_hash(launch), 7: packfile.key_hash(twin)})
        directlink.set_tiers([twin, launch], tiers)
        self.assertEqual((launch['tier'], twin['tier']), (0, 0))
        alone = self.state(1, b'alone', key=self.KEYS[0])
        directlink.set_tiers([alone], tiers)
        self.assertEqual(alone['tier'], 1)

    def test_the_header_names_tier_zeros_part_and_fast_keys_go_by_tier(self):
        first, second = (packfile.fnv(k.encode()) for k in self.KEYS)
        pipes = [dict(record(0), key=self.KEYS[1], tier=0), dict(record(0), key=self.KEYS[0], tier=1)]
        section, launch = packfile.fast_key_bytes({9: second, 3: first}, pipes)
        self.assertEqual(launch, 1)
        self.assertEqual([struct.unpack_from('<QII', section, 16 * i) for i in range(2)], [(9, 0, 1), (3, 1, 1)])
        programs = [{'cbufs': [], 'heap_offset': 0, 'program': 0x30, 'bytes': 256, 'data_offset': 0, 'data_bytes': 0}]
        pack = packfile.Pack(pipes, programs * 2, [], {}, [], bytearray(0x200), [], tier0_programs=1, tier0_heap=0x100)
        pack.fast_keys = {9: second, 3: first}
        data, _ = packfile.pack_bytes(pack)
        sections = struct.unpack_from(f'<{2 * packfile.SECTIONS}Q', data, 24)
        tier0 = struct.unpack_from(f'<{packfile.SECTIONS}Q', data, 24 + 16 * packfile.SECTIONS + 16)
        self.assertEqual(tier0[0], packfile.PROGRAM_BYTES)
        self.assertEqual(tier0[1], directlink.PIPELINE_BYTES)
        self.assertEqual(tier0[7], 0x100)
        self.assertEqual(tier0[15], 16)
        self.assertEqual(sections[2 * 7] % packfile.TIER_ALIGN, 0)
        self.assertIn('heap 0 (0)', packfile.sections_line(data))

    def test_a_programs_facts_go_from_its_record_into_the_pack(self):
        words = 7 | 2 << 8 | WRITES_MEMORY << 16
        blob = b''.join(struct.pack('<I', len(b)) + b for b in (bytes(84), b'', b'', b'')) + struct.pack('<I', words)
        made = directlink.program(directlink.Reader(blob))
        self.assertEqual((made['gprs'], made['bound_textures'], made['facts']), (7, 2, WRITES_MEMORY))
        heap, index, recs = bytearray(), {}, []
        packfile.place_graphics('p', {'p': made}, heap, index, recs)
        data, _ = packfile.pack_bytes(packfile.Pack([], recs, [], {}, [], heap, []))
        programs = struct.unpack_from('<Q', data, 24)[0]
        self.assertEqual(struct.unpack_from('<I', data, programs + 44)[0], WRITES_MEMORY)

    def test_tier_ones_records_before_tier_zeros_fail_the_link(self):
        pipes = [dict(record(0), key=self.KEYS[0], tier=1), dict(record(0), key=self.KEYS[1], tier=0)]
        with self.assertRaisesRegex(ValueError, 'tier'):
            packfile.pack_bytes(packfile.Pack(pipes, [], [], {}, [], bytearray(), []))

    def test_a_split_fragment_shared_by_both_tiers_is_laid_out_once_in_tier_zero(self):
        launch = self.state(0, b'shared-split', key=self.KEYS[0])
        cold = self.state(1, b'other-vs', key=self.KEYS[1])
        cold['programs'][1] = launch['programs'][1]
        cold['probe']['records'][0]['prog'][5] = launch['probe']['records'][0]['prog'][5]
        whole = directlink.program_id(launch['programs'][1])
        split_fs = {'image': bytes(80) + b'per-pixel', 'data': b'', 'cbufs': [6], 'far_ubos': []}
        prologue = {'image': bytes(80) + b'prologue', 'data': b'', 'cbufs': [], 'far_ubos': []}
        fs_id, pro_id = directlink.program_id(split_fs), directlink.program_id(prologue)
        programs = {directlink.program_id(p): p for rec in (launch, cold) for p in rec['programs']} | {
            fs_id: split_fs,
            pro_id: prologue,
        }
        found = {
            whole: {
                'fs': fs_id,
                'prologue': pro_id,
                'frontier_bytes': 16,
                'frontier_cbuf': 0,
                'prologue_cbufs': 1,
                'shader_data_cbuf': 0xFF,
                'ubo_cbufs': [],
                'threshold': 10,
                'batch_version': 0,
                'batch_threads': 0,
                'qmd': [0] * 64,
                'cost': dict.fromkeys(packfile.SPLIT_COST_FIELDS, 0),
            }
        }
        (_, _, recs), split_recs, launch_programs, _ = directlink.lay_out_tiers([launch, cold], programs, found, [])
        self.assertEqual(len(split_recs), 1)
        self.assertLess(split_recs[0]['whole'], launch_programs)
        self.assertLess(split_recs[0]['fs'], launch_programs)
        self.assertLess(split_recs[0]['prologue'], launch_programs)
        self.assertEqual(len(recs), 5)


if __name__ == '__main__':
    unittest.main()
