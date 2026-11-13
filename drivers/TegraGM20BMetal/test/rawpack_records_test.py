#!/usr/bin/env python3
"""What the raw pack generator keeps of a probe.

A texture3d argument is an entry with no slot, a state holds up to 32 set-0 entries, the buffer m2v binds of its own
beside an 8-bit sRGB target is named as such, and a storage buffer's SlotBindOffset residual word is named by its code
(raw/pack/probe_bind.h, the runtime's raw_residual_test); a compute kernel's direct record (kernels.py) is the
PackKernel it holds, its samplers numbered among the pack's.
"""

import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'raw', 'pack'))
import kernels
import directlink
import layouts
import mkpack
import packfile


def handle(tex: tuple | None) -> dict:
    return {'off': 228, 'kind': 'handle', 'tex': tex, 'smp': None}


class RecordsTest(unittest.TestCase):
    def test_dynamic_blend_metadata_round_trip(self):
        rec = dict(
            key='samples=1 c0=554:15:1:11:0:0:1:0:0',
            vs=0,
            fs=1,
            blob=[],
            entries=[],
            push=[(0x700 + 4 * i, i) for i in range(32)],
            vbs=[],
            set_size=0,
            fetch=True,
            form='barrier',
            push_metadata=True,
            blend_color_word=5,
            half_blend_color_word=29,
        )
        data = packfile.pipeline_bytes(rec, 0, 0)
        self.assertEqual(len(data), directlink.PIPELINE_BYTES)
        programs = [dict(image=bytes([i]), data=b'', cbufs=[]) for i in range(2)]
        ids = {directlink.program_id(p): i for i, p in enumerate(programs)}
        record = dict(
            pipeline=data,
            key=rec['key'],
            programs=programs,
            samplers=[],
            probe=dict(index=0, form='barrier', targets=1, records=[dict(regs={})]),
        )
        back = directlink.pipeline_of(record, [], ids, [])
        self.assertEqual(packfile.pipeline_bytes(back, 0, 0), data)
        self.assertTrue(back['push_metadata'])
        self.assertEqual((back['blend_color_word'], back['half_blend_color_word']), (5, 29))
        record['pipeline'] = data[:-4]
        with self.assertRaisesRegex(ValueError, 'obsolete layout'):
            directlink.pipeline_of(record, [], ids, [])

    def test_vertex_streams_past_stream_0_round_trip_and_a_bind_stride_is_refused(self):
        rec = dict(
            key='samples=1 c0=80:15:0 a0=30:0:1:12:1:1 a1=31:0:4:16:1:1',
            vs=0,
            fs=1,
            blob=[],
            entries=[],
            push=[],
            vbs=[{'stream': 0, 'slot': 1, 'stride': 12}],
            set_size=0,
            fetch=False,
            vertex_streams=[5],
        )
        data = packfile.pipeline_bytes(rec, 0, 0)
        v21 = len(data) - packfile.PIPELINE_V22.size - packfile.PIPELINE_V21.size
        self.assertEqual(data[v21 : v21 + packfile.PIPELINE_V21.size], bytes(16) + bytes([5]) + bytes(7))
        programs = [dict(image=bytes([i]), data=b'', cbufs=[]) for i in range(2)]
        ids = {directlink.program_id(p): i for i, p in enumerate(programs)}
        record = dict(
            pipeline=data,
            key=rec['key'],
            programs=programs,
            samplers=[],
            probe=dict(index=0, form='', targets=1, records=[dict(regs={0x1C00: 0x1000 | 12, 0x1C10: 0x1000 | 16})]),
        )
        back = directlink.pipeline_of(record, [], ids, [])
        self.assertEqual(back['vertex_streams'], [5])
        self.assertEqual(packfile.pipeline_bytes(back, 0, 0), data)
        record['pipeline'] = packfile.pipeline_bytes(dict(rec, vertex_streams=[5 | packfile.STREAM_BIND_STRIDE]), 0, 0)
        with self.assertRaisesRegex(ValueError, 'runtime-only'):
            directlink.pipeline_of(record, [], ids, [])
        # a vertex function choosing its slice and viewport: the two push words each draw writes are the pack's
        record['pipeline'] = packfile.pipeline_bytes(dict(rec, array_length_word=3, viewport_count_word=4), 0, 0)
        self.assertEqual(
            record['pipeline'][v21 : v21 + packfile.PIPELINE_V21.size],
            bytes(7) + bytes([3]) + bytes(8) + bytes([5]) + bytes(6) + bytes([4]),
        )
        back = directlink.pipeline_of(record, [], ids, [])
        self.assertEqual((back['array_length_word'], back['viewport_count_word']), (3, 4))
        self.assertEqual(packfile.pipeline_bytes(back, 0, 0), record['pipeline'])

    def test_a_two_plane_record_keeps_its_sampled_slots_and_flags(self):
        rec = dict(
            key='samples=1 c0=550:15:0',
            vs=0,
            fs=1,
            blob=[],
            entries=[],
            push=[],
            vbs=[],
            set_size=0,
            fetch=False,
            turn_refused=True,
            two_plane=True,
            xr_slots=0x8001,
        )
        data = packfile.pipeline_bytes(rec, 0, 0)
        self.assertEqual(data[-4:], struct.pack('<HH', 0x8001, 0))
        self.assertEqual(directlink.PIPELINE.unpack_from(data)[-1] & 0xE0, 0xC0)
        programs = [dict(image=bytes([i]), data=b'', cbufs=[]) for i in range(2)]
        ids = {directlink.program_id(p): i for i, p in enumerate(programs)}
        record = dict(
            pipeline=data,
            key=rec['key'],
            programs=programs,
            samplers=[],
            probe=dict(index=0, form='', targets=0b11, records=[dict(regs={})]),
        )
        back = directlink.pipeline_of(record, [], ids, [])
        self.assertEqual((back['turn_refused'], back['two_plane'], back['xr_slots']), (True, True, 0x8001))
        self.assertEqual(packfile.pipeline_bytes(back, 0, 0), data)

    def test_a_vertex_stream_fetches_at_its_layouts_stride_alone(self):
        rec = dict(
            key='samples=1 c0=80:15:0 a0=30:0:1:12:1:1 a1=31:0:4:4100:1:1',
            vs=0,
            fs=1,
            blob=[],
            entries=[],
            push=[],
            vbs=[{'stream': 0, 'slot': 1, 'stride': 12}],
            set_size=0,
            fetch=False,
            vertex_streams=[5],
        )
        programs = [dict(image=bytes([i]), data=b'', cbufs=[]) for i in range(2)]
        ids = {directlink.program_id(p): i for i, p in enumerate(programs)}
        record = dict(
            pipeline=packfile.pipeline_bytes(rec, 0, 0),
            key=rec['key'],
            programs=programs,
            samplers=[],
            probe=dict(
                index=0, form='', targets=1, records=[dict(regs={0x1C00: 0x1000 | 12, 0x1C10: 0x1000 | (4100 & 0xFFF)})]
            ),
        )
        with self.assertRaisesRegex(ValueError, 'vertex stream 1 of buffer 4'):
            directlink.pipeline_of(record, [], ids, [])
        record['pipeline'] = packfile.pipeline_bytes(dict(rec, vertex_streams=[]), 0, 0)
        self.assertEqual(directlink.pipeline_of(record, [], ids, [])['vertex_streams'], [])
        record['probe']['records'][0]['regs'][0x1C00] = 0x1000 | 8
        with self.assertRaisesRegex(ValueError, 'vertex stream 0 of buffer 1'):
            directlink.pipeline_of(record, [], ids, [])

    def test_record_v15_tail_round_trip(self):
        uniform = bytes([0, 1, 2, 9]) + b'\xff\x00\x00\x00' * 7
        rec = dict(
            key='samples=1 c0=80:15:0',
            vs=0,
            fs=1,
            blob=[],
            entries=[handle(('fetch', 0))],
            push=[],
            vbs=[],
            set_size=256,
            fetch=True,
            bound_entries=1,
            uniform=uniform,
            fetch_rung=[1, 0, 0, 0, 0, 0, 0, 0],
        )
        programs = [dict(image=bytes([i]), data=b'', cbufs=[], gprs=24, bound_textures=0) for i in range(3)]
        ids = {directlink.program_id(p): i for i, p in enumerate(programs)}
        data = packfile.pipeline_bytes(dict(rec, variants=[dict(stage=1, assume=1, program=2)]), 0, 0, 5)
        self.assertEqual(len(data), directlink.PIPELINE_BYTES)
        record = dict(
            pipeline=data,
            key=rec['key'],
            programs=programs[:2],
            samplers=[],
            variants=[dict(assume=1, program=programs[2])],
            probe=dict(index=0, form='', targets=1, records=[dict(regs={})]),
        )
        back = directlink.pipeline_of(record, [], ids, [])
        self.assertEqual(back['variants'], [dict(stage=1, assume=1, program=2)])
        self.assertEqual((back['bound_entries'], back['uniform'], back['fetch_rung']), (1, uniform, rec['fetch_rung']))
        self.assertEqual(back['memory_entries'], packfile.ALL_ENTRIES)
        self.assertEqual(
            directlink.pipeline_of(
                dict(record, pipeline=packfile.pipeline_bytes(dict(rec, memory_entries=5), 0, 0)), [], ids, []
            )['memory_entries'],
            5,
        )
        self.assertEqual(packfile.pipeline_bytes(back, 0, 0, 5), data)
        pipes, _, _, variants = packfile.pipeline_sections([dict(back, key=rec['key']), dict(back, key=rec['key'])])
        self.assertEqual(len(variants), 16)
        v15 = (
            directlink.PIPELINE_BYTES * 2
            - packfile.PIPELINE_V22.size
            - packfile.PIPELINE_V21.size
            - packfile.PIPELINE_V20.size
            - packfile.PIPELINE_V18.size
            - packfile.PIPELINE_V15.size
        )
        self.assertEqual(struct.unpack_from('<I', pipes, v15), (1,))

    def test_argument_buffer_tail_round_trip(self):
        tables = [{'off': 32, 'kind': 'texture_table', 'stage': 2}, {'off': 48, 'kind': 'sampler_table', 'stage': 2}]
        heap = [{'class': 0x25, 'offset': 0}, {'class': 2, 'offset': 0x8000}]
        rec = {
            'key': 'samples=1 c0=70:15:0',
            'vs': 0,
            'fs': 1,
            'blob': [],
            'entries': tables,
            'push': [],
            'vbs': [],
            'set_size': 64,
            'fetch': False,
            'root_table_word': [0, 17],
            'status_word': [0, 19],
            'root_slots': [0, 5],
            'heap': heap,
            'heap_bytes': 0x10000,
        }
        data = packfile.pipeline_bytes(rec, 0, 0)
        self.assertEqual(len(data), directlink.PIPELINE_BYTES)
        programs = [{'image': bytes([i]), 'data': b'', 'cbufs': []} for i in range(2)]
        ids = {directlink.program_id(p): i for i, p in enumerate(programs)}
        probe = {'index': 0, 'form': '', 'targets': 1, 'records': [{'regs': {}}]}
        record = {'pipeline': data, 'key': rec['key'], 'programs': programs, 'samplers': [], 'probe': probe}
        back = directlink.pipeline_of(record, [], ids, [])
        self.assertEqual(back['entries'], tables)
        self.assertEqual((back['root_table_word'], back['status_word'], back['root_slots']), ([0, 17], [0, 19], [0, 5]))
        self.assertEqual((back['heap'], back['heap_bytes']), (heap, 0x10000))
        self.assertEqual(packfile.pipeline_bytes(back, 0, 0), data)

    def test_argument_tables_round_trip_and_take_their_words_in_the_blob_section(self):
        entries = [{'off': 0, 'kind': 'spill_table', 'stage': 1}, {'off': 16, 'kind': 'spill_table', 'stage': 2}]
        rec = {
            'key': 'samples=1 c0=70:15:0',
            'vs': 0,
            'fs': 1,
            'blob': [],
            'entries': entries,
            'push': [],
            'vbs': [],
            'set_size': 32,
            'fetch': False,
            'spill_entry': [1, 2],
            'spill': [[7, 8], [1, 2, 3, 4]],
            'spill_at': 5,
        }
        data = packfile.pipeline_bytes(rec, 0, 0)
        self.assertEqual(len(data), directlink.PIPELINE_BYTES)
        v20 = len(data) - packfile.PIPELINE_V22.size - packfile.PIPELINE_V21.size - packfile.PIPELINE_V20.size
        self.assertEqual(packfile.PIPELINE_V20.unpack_from(data, v20)[24:], (1, 2, 2, 4, 5))
        programs = [{'image': bytes([i]), 'data': b'', 'cbufs': []} for i in range(2)]
        ids = {directlink.program_id(p): i for i, p in enumerate(programs)}
        probe = {'index': 0, 'form': '', 'targets': 1, 'records': [{'regs': {}}]}
        record = {
            'pipeline': data,
            'key': rec['key'],
            'programs': programs,
            'samplers': [],
            'probe': probe,
            'spill': [7, 8, 1, 2, 3, 4],
        }
        back = directlink.pipeline_of(record, [], ids, [])
        self.assertEqual((back['entries'], back['spill_entry'], back['spill']), (entries, [1, 2], rec['spill']))
        self.assertEqual(packfile.pipeline_bytes(dict(back, spill_at=5), 0, 0), data)
        pipes, blob, _, _ = packfile.pipeline_sections(
            [dict(back, blob=[(0x1A00, 9)]), dict(rec, spill_entry=[0, 0], spill=[[], []], entries=[])]
        )
        self.assertEqual(blob, [0x1A00, 9, 7, 8, 1, 2, 3, 4])
        v20_end = packfile.PIPELINE_V22.size + packfile.PIPELINE_V21.size + packfile.PIPELINE_V20.size
        tails = [
            packfile.PIPELINE_V20.unpack_from(pipes, (i + 1) * directlink.PIPELINE_BYTES - v20_end) for i in range(2)
        ]
        self.assertEqual((tails[0][24:], tails[1][24:]), ((1, 2, 2, 4, 2), (0, 0, 0, 0, 0)))

    def test_record_v22_written_formats_round_trip(self):
        rec = dict(
            key='samples=1 c0=80:15:0 | v a | f b | wtex=1:23',
            vs=0,
            fs=1,
            blob=[],
            entries=[handle(('slot', 1))],
            push=[],
            vbs=[],
            set_size=256,
            fetch=False,
            written_format=[0, 23] + [0] * 14,
        )
        programs = [dict(image=bytes([i]), data=b'', cbufs=[], gprs=24, bound_textures=0) for i in range(2)]
        ids = {directlink.program_id(p): i for i, p in enumerate(programs)}
        data = packfile.pipeline_bytes(rec, 0, 0)
        record = dict(
            pipeline=data,
            key=rec['key'],
            programs=programs,
            samplers=[],
            probe=dict(index=0, form='', targets=1, records=[dict(regs={})]),
        )
        back = directlink.pipeline_of(record, [], ids, [])
        self.assertEqual(back['written_format'], rec['written_format'])
        self.assertEqual(packfile.pipeline_bytes(back, 0, 0), data)
        self.assertEqual(packfile.variant_name(rec['key']), ' | wtex=1:23')
        self.assertEqual(
            packfile.variant_name(rec['key'].replace('wtex=1:23', 'tests=2 | wtex=0:25,1:23')),
            ' | tests=2 | wtex=0:25,1:23',
        )

    def test_the_report_counts_variant_records_by_form_and_names_each_sampled_two_plane_one(self):
        state = 'samples=1 c0=550:15:0:1:0:0:1:0:0 | v a | f b'
        cubic = ' | cubic s0:0000n t0:550 | xrs=4'
        names = ['', ' | xrs=2', ' | xrs=2', ' | xr2p | xrs=2', ' | tests=1 | xrs=2', cubic, ' | wtex=1:23']
        lines = directlink.variant_report([{'key': state + n} for n in names])
        self.assertEqual(
            lines[0],
            '6 variant records: 1 of a test class, 1 of cubic samplers, 1 of two-plane passes, '
            '5 of sampled two-plane textures, 1 of written formats',
        )
        self.assertEqual(
            lines[1:],
            [
                f'  sampled two-plane variant (xrs=2): {state} | xrs=2',
                f'  sampled two-plane variant ({cubic[3:]}): {state}{cubic}',
            ],
        )

    def test_a_variant_record_lies_under_its_states_key_and_is_named(self):
        own = {
            'key': 'samples=1 ds=260/260 c0=80:15:0 | v a | f b',
            'vs': 0,
            'fs': 1,
            'blob': [],
            'entries': [],
            'push': [],
            'vbs': [],
            'set_size': 0,
            'fetch': True,
        }
        tests = {**own, 'key': own['key'] + ' | tests=3'}
        self.assertEqual(packfile.key_hash(tests), packfile.key_hash(own))
        pipes, _, strings, _ = packfile.pipeline_sections([own, tests])
        records = [pipes[i * directlink.PIPELINE_BYTES : (i + 1) * directlink.PIPELINE_BYTES] for i in range(2)]
        flags = [directlink.PIPELINE.unpack_from(r)[-1] for r in records]
        self.assertEqual((flags[0] & 16, flags[1] & 16), (0, 16))
        v18 = (
            directlink.PIPELINE_BYTES
            - packfile.PIPELINE_V22.size
            - packfile.PIPELINE_V21.size
            - packfile.PIPELINE_V20.size
            - packfile.PIPELINE_V18.size
        )
        name = struct.unpack_from('<I', records[1], v18 - 4)[0]
        self.assertEqual(strings[name:].split(b'\0')[0], b' | tests=3')

    def test_ordered_storage_metadata_round_trip_and_explicit_registers(self):
        rec = dict(
            key='samples=1 c0=81:15:0:1:0:0:1:0:0',
            vs=0,
            fs=1,
            blob=[],
            entries=[handle(('fetch', 0))],
            push=[],
            vbs=[],
            set_size=256,
            fetch=True,
            storage_mask=1,
            storage_write_mask=1,
            storage_samples=1,
            storage_tests=2,
            storage_tic=[0x24912] + [0] * 7,
            storage_blend_mask=1,
            storage_blend_model=71,
        )
        data = packfile.pipeline_bytes(rec, 0, 0)
        programs = [dict(image=bytes([i]), data=b'', cbufs=[]) for i in range(2)]
        ids = {directlink.program_id(p): i for i, p in enumerate(programs)}
        record = dict(
            pipeline=data,
            key=rec['key'],
            programs=programs,
            samplers=[],
            probe=dict(index=0, form='', targets=1, records=[dict(regs={0x0210: 1, 0x0754: 1, 0x1224: 2, 0x1A00: 0})]),
        )
        back = directlink.pipeline_of(record, [], ids, [])
        self.assertEqual(back['storage_mask'], 1)
        self.assertEqual(back['storage_write_mask'], 1)
        self.assertEqual(back['storage_samples'], 1)
        self.assertEqual(back['storage_tests'], 2)
        self.assertEqual((back['storage_blend_mask'], back['storage_blend_model']), (1, 71))
        self.assertEqual(back['storage_tic'], rec['storage_tic'])
        self.assertEqual(dict(back['blob']), {0x0210: 1, 0x0754: 1, 0x1224: 2, 0x1A00: 0})
        self.assertEqual(directlink.PIPELINE.unpack_from(packfile.pipeline_bytes(back, 0, 0))[-1] & 8, 8)

    def test_legacy_records_do_not_claim_push_metadata(self):
        rec = dict(key='samples=1', vs=0, fs=1, blob=[], entries=[], push=[], vbs=[], set_size=0, fetch=False)
        data = packfile.pipeline_bytes(rec, 0, 0)
        self.assertEqual(directlink.PIPELINE.unpack_from(data)[-1] & 4, 0)
        v21 = len(data) - packfile.PIPELINE_V22.size - packfile.PIPELINE_V21.size
        v20 = v21 - packfile.PIPELINE_V20.size
        self.assertEqual(data[v20 - 8 : v20 - 4], bytes(4))
        self.assertEqual(struct.unpack_from('<I', data, v20 - 4), (packfile.ALL_ENTRIES,))
        self.assertEqual(data[v21:], bytes(packfile.PIPELINE_V21.size + packfile.PIPELINE_V22.size))

    def test_shader_test_views_and_push_placements_round_trip(self):
        rec = dict(
            key='samples=4 ds=260/260 c0=80:15:0',
            vs=0,
            fs=1,
            blob=[],
            entries=[handle(('fetch', 0)), handle(('depth_test', 0)), handle(('stencil_test', 0))],
            push=[(0x700 + 4 * i, i) for i in range(8)],
            vbs=[],
            set_size=256,
            fetch=True,
            push_metadata=True,
            storage_mask=1,
            storage_write_mask=1,
            storage_samples=4,
            storage_tests=4,
            storage_tic=[0x24912] + [0] * 7,
            depth_test_word=1,
            stencil_test_word=5,
            test_flags=3,
            depth_tic=100,
            stencil_tic=13,
        )
        data = packfile.pipeline_bytes(rec, 0, 0)
        programs = [dict(image=bytes([i]), data=b'', cbufs=[]) for i in range(2)]
        ids = {directlink.program_id(p): i for i, p in enumerate(programs)}
        record = dict(
            pipeline=data,
            key=rec['key'],
            programs=programs,
            samplers=[],
            probe=dict(index=0, form='', targets=1, records=[dict(regs={0x0754: 20, 0x1224: 2})]),
        )
        back = directlink.pipeline_of(record, [], ids, [])
        self.assertEqual(
            [entry['tex'] for entry in back['entries']], [('fetch', 0), ('depth_test', 0), ('stencil_test', 0)]
        )
        for field in (
            'depth_test_word',
            'stencil_test_word',
            'test_flags',
            'depth_tic',
            'stencil_tic',
            'storage_samples',
            'storage_tests',
        ):
            self.assertEqual(back[field], rec[field], field)
        back['blob'] = []
        self.assertEqual(packfile.pipeline_bytes(back, 0, 0), data)

    def test_only_known_own_shader_pairs_use_alias_keys(self):
        own = 'samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0 | v resolve_vert  | f resolve_bgra8_frag'
        self.assertEqual(packfile.key_hash({'key': own}), packfile.fnv(b'raw.resolve.resolve_bgra8_frag'))
        mip = 'samples=1 topo=0 ds=0/0 c0=43:15:0:1:0:0:1:0:0 | v mip_vert  | f mip_bgr5a1_frag'
        self.assertEqual(packfile.key_hash({'key': mip}), packfile.fnv(b'raw.mip.mip_bgr5a1_frag'))
        variant = {'key': own + ' | tests=1'}
        self.assertEqual(packfile.key_hash(variant), packfile.fnv(b'raw.resolve.resolve_bgra8_frag'))
        for key in (
            own + ' | l RenderBox',
            own + ' | air 1234',
            own.replace('resolve_bgra8_frag', 'resolve_sample_fragment #0:36:01'),
            own.replace('resolve_vert', 'plane__vertex'),
        ):
            with self.subTest(key=key):
                self.assertEqual(packfile.key_hash({'key': key}), packfile.fnv(key.encode()))
        a = own.replace('resolve_vert', 'plane__vertex').replace(
            'resolve_bgra8_frag', 'resolve_sample_fragment #0:36:01'
        )
        b = a.replace('#0:36:01', '#0:36:02')
        self.assertNotEqual(packfile.key_hash({'key': a}), packfile.key_hash({'key': b}))

    def test_both_stencil_routes_states_are_found_by_their_names(self):
        for form in ('s8', 'd32fs8', 'ms_s8', 'ms_d32fs8', 'ms_d32fs8_min', 'ms_d32fs8_max'):
            for route in ('', '_colour'):
                name = f'stencil_bits_{form}{route}_frag'
                key = f'samples=1 topo=0 ds=0/0 c0=103:4:0:1:0:0:1:0:0 | v stencil_bits_vert | f {name}'
                with self.subTest(name=name):
                    self.assertEqual(packfile.key_hash({'key': key}), packfile.fnv(f'raw.stencil_bits.{name}'.encode()))

    def test_a_texture3d_argument_is_an_entry_with_no_slot(self):
        problems: list[str] = []
        ent = mkpack.handle_entry((228, 'handle', ('tex3d', 'R102'), None, 0), {}, [], problems)
        self.assertEqual(problems, [])
        self.assertEqual(ent['tex'], ('tex3d', 0xFF))
        off, kind, stage, slot = struct.unpack_from('<HBBB', packfile.entry_bytes([ent]))
        self.assertEqual((off, kind, stage, slot), (228, 4, 2, 0xFF))

    def test_a_vertex_handle_keeps_its_stage(self):
        vertex = dict(handle(('tex3d', 0)), smp=('slot', 7), stage='v', compare=False)
        data = packfile.entry_bytes([vertex, handle(('slot', 20))])
        self.assertEqual(struct.unpack_from('<HBBBBBB', data, 0), (228, 4, 1, 0, 1, 7, 0))
        self.assertEqual(struct.unpack_from('<HBBBBBB', data, 8), (228, 2, 2, 20, 0, 0, 0))
        pipeline = bytes(directlink.PIPELINE.size) + data
        back = directlink.entries_of(pipeline, 2, [], [])
        self.assertEqual(back[0], vertex)
        self.assertNotIn('stage', back[1])
        self.assertEqual(packfile.entry_bytes(back), data)

    def test_a_target_between_two_a_state_has_is_foreign_to_it(self):
        mask = mkpack.targets('samples=1 c0=25:15 c2=25:15 c3=25:15 | v FusionVert')
        self.assertEqual(mask, 0b1101)
        self.assertEqual([mkpack.foreign(0x1A00 + 4 * j, mask) for j in range(5)], [False, True, False, False, True])
        self.assertEqual([mkpack.foreign(0x1E04 + 0x20 * j, mask) for j in range(4)], [False, True, False, False])

    def test_a_slot_m2v_binds_its_compare_header_at_is_a_compared_entry(self):
        problems: list[str] = []
        lod2 = 2 * 256 << 12  # the probe's tag of sampler slot 2: max LOD 2, nearest, repeating U
        tsc = {5: [0x26090 | 1 << 9 | 1 << 10, 0x291, lod2, 0, 0, 0, 0, 0], 6: [0x26090, 0x291, lod2, 0, 0, 0, 0, 0]}
        compared = mkpack.handle_entry((64, 'handle', ('tex', 0), ('smp', 2), 5 << 20), tsc, [], problems)
        plain = mkpack.handle_entry((72, 'handle', ('tex', 1), ('smp', 2), 6 << 20), tsc, [], problems)
        self.assertEqual(problems, [])
        self.assertEqual((compared['compare'], plain['compare']), (True, False))
        ents = packfile.entry_bytes([compared, plain])
        self.assertEqual([struct.unpack_from('<HBBBBBB', ents, 8 * i)[6] for i in range(2)], [1, 0])
        pipeline = bytes(directlink.PIPELINE.size) + bytes(ents)
        read = directlink.entries_of(pipeline, 2, [], [])
        self.assertEqual([e['compare'] for e in read], [True, False])

    def test_a_pipeline_holds_32_entries(self):
        ents = packfile.entry_bytes([handle(('slot', 3))] * 27)
        self.assertEqual(len(ents), 8 * 32)
        with self.assertRaises(ValueError):
            packfile.entry_bytes([handle(('slot', 3))] * 33)

    def test_srgb_targets(self):
        key = 'samples=1 topo=0 ds=0/0 c0=81:15:0:1:0:0:1:0:0 c1=115:15:0:1:0:0:1:0:0  | v f'
        self.assertTrue(layouts.srgb_target(key))
        self.assertTrue(layouts.pointer_problem(key, 176, 0).startswith("a buffer of m2v's own at 176"))
        self.assertFalse(layouts.srgb_target('samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v f'))

    def test_probe_offsets_are_probe_binds(self):
        self.assertEqual(mkpack.bind_offset('v', 1), 24)
        self.assertEqual(mkpack.storage_code('v', 1), 8)
        self.assertEqual(mkpack.bind_offset('f', 0), 193)
        self.assertEqual(mkpack.bind_offset('f', 30), 233)
        offsets = {mkpack.bind_offset(st, slot) for st in 'vf' for slot in range(31)}
        self.assertEqual(len(offsets), 62)

    def test_a_storage_buffer_takes_its_codes_word(self):
        index = (0, 7)
        sizes = {(0, 7, 16): (16, 4096), (0, 7, 32): (256, 4096), (0, 7, 48): (16, 4096)}
        words = {mkpack.storage_code('v', 1): 0x704, mkpack.bind_offset('v', 2): 0x708}
        storage = mkpack.buffer_entry((16, 'ubo', 'v', 1, 0), index, sizes, words, set())
        uniform = mkpack.buffer_entry((32, 'ubo', 'v', 2, 0), index, sizes, words, set())
        self.assertEqual((storage['ssbo'], storage['residual']), (True, 0x704))
        self.assertEqual((uniform['ssbo'], uniform['residual']), (False, 0x708))
        self.assertEqual(packfile.buffer_kind(storage), 1 | 2 << 2)
        self.assertEqual(packfile.buffer_kind(uniform), 3 << 2)
        shared = mkpack.buffer_entry((16, 'ubo', 'v', 1, 0), index, sizes, words, {mkpack.storage_code('v', 1)})
        self.assertIsNone(shared['residual'])
        self.assertEqual(packfile.buffer_kind(shared), 1)


def record(key: str, entries: list[tuple[int, int, int, int]], samplers: list[list[int]], outcome: int = 0) -> dict:
    """Make a DKRN record with region 4, set 0x40 and push word 2."""
    qmd = [0] * 64
    qmd[20] = 0x7
    words = [0] * 32
    words[2] = 0x11
    body = kernels.HEAD.pack(0, 0, 0, len(entries), 4) + struct.pack('<64I', *qmd)
    body += kernels.COUNTS.pack(0x40, 0, 1 << 2) + struct.pack('<32I', *words)
    body += b''.join(kernels.ENTRY.pack(*e) for e in entries) + bytes(
        kernels.ENTRY.size * (kernels.ENTRIES - len(entries))
    )
    body += bytes(packfile.KERNEL_V20.size + packfile.KERNEL_V21.size)
    rec = {'outcome': outcome, 'key': key, 'why': 'a texture slot the dispatch leaves unbound at 0' if outcome else ''}
    if not outcome:
        rec.update(kernel=body, program='p', samplers=samplers)
    return rec


class KernelRecordsTest(unittest.TestCase):
    def test_a_states_answers_are_packed_once_ascending_and_must_agree(self):
        state = 'aa | k  | max=0 tgm=0'
        recs = [dict(record(state + ' | tg=8,8,1 | tgmem= | tex= | smp=', [], []), facts=(512, 32, 64))]
        recs.append(dict(record(state + ' | tg=16,16,1 | tgmem= | tex= | smp=', [], []), facts=(512, 32, 64)))
        recs.append(
            dict(record('bb | k  | max=0 tgm=0 | tg=1,1,1 | tgmem= | tex= | smp=', [], []), facts=(1024, 32, 0))
        )
        facts = kernels.state_facts(recs)
        self.assertEqual(len(facts), 2)
        section = packfile.compute_facts_bytes(facts)
        rows = [struct.unpack_from('<QIIII', section, 24 * i) for i in range(len(section) // 24)]
        self.assertEqual(
            [r[0] for r in rows], sorted(packfile.fnv(k.encode()) for k in (state, 'bb | k  | max=0 tgm=0'))
        )
        self.assertIn((packfile.fnv(state.encode()), 512, 32, 64, 0), rows)
        recs.append(dict(record(state + ' | tg=4,4,1 | tgmem= | tex= | smp=', [], []), facts=(256, 32, 64)))
        with self.assertRaises(ValueError):
            kernels.state_facts(recs)

    def test_a_record_is_the_pack_kernel_it_holds(self):
        self.assertEqual(len(record('k', [], [])['kernel']), kernels.KERNEL_BYTES)
        (k,), skipped = kernels.from_records([record('k', [(0, kernels.BUFFER, 2, 64 | 4 << 24)], [])], [])
        self.assertEqual(skipped, [])
        self.assertEqual((k['region'], k['set_bytes'], k['push'], k['qmd'][20]), (4, 0x40, {2: 0x11}, 0x7))
        self.assertEqual(k['entries'][0], {'off': 0, 'kind': kernels.BUFFER, 'slot': 2, 'window': 64, 'residual': 3})
        self.assertEqual(packfile.kernel_window(k['entries'][0]), 64 | 4 << 24)

    def test_a_buffer_without_a_residual_word_names_none(self):
        (k,), _ = kernels.from_records([record('k', [(0, kernels.BUFFER, 2, 32)], [])], [])
        self.assertIsNone(k['entries'][0]['residual'])
        self.assertEqual(packfile.kernel_window(k['entries'][0]), 32)

    def test_samplers_are_numbered_among_the_packs(self):
        ours, theirs = [1, 2, 3, 4, 5, 6, 7, 8], [9, 9, 9, 9, 9, 9, 9, 9]
        constexpr_tsc = [theirs]
        sampler = (16, kernels.SAMPLER, kernels.CONSTEXPR | 1, 0)
        (a, b), _ = kernels.from_records(
            [record('a', [sampler], [theirs, ours]), record('b', [sampler], [[0] * 8, ours])], constexpr_tsc
        )
        self.assertEqual(constexpr_tsc, [theirs, ours])
        self.assertEqual(a['entries'][0]['slot'], kernels.CONSTEXPR | 1)
        self.assertEqual(b['entries'][0]['slot'], kernels.CONSTEXPR | 1)

    def test_a_refused_shape_and_a_key_made_twice_are_left_out(self):
        made, skipped = kernels.from_records([record('a', [], []), record('a', [], []), record('b', [], [], 1)], [])
        self.assertEqual([k['key'] for k in made], ['a'])
        self.assertEqual(skipped, [('b', 'a texture slot the dispatch leaves unbound at 0')])


if __name__ == '__main__':
    unittest.main()
