#!/usr/bin/env python3
"""The pack's link step stays linear in programs and states (pack/packfile.py place_graphics, pack/mkpack.py
lay_out_programs) and hashes each program once (pack/directlink.py program_id): the closed Core Animation set makes
the link place well over 100,000 programs and name them over a million times.

Each case does at 40,000 what a link does: a scan of every placed record per new program took about 14 s here at
this size and grows with its square; a probe takes milliseconds. The ceiling leaves a remote worker 20 times the
linear time and is far below the quadratic one.
"""

import json
import os
import re
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'pack'))
import directlink  # noqa: E402
import mkpack  # noqa: E402
import packfile  # noqa: E402
import segments  # noqa: E402

COUNT = 40_000
CEILING_S = 3.0
VARIANT_NAME_RE = re.compile(r'( \| cubic [^|]*?)?( \| tests=\d+)?( \| xrs=\d+)?( \| wtex=[\d:,]+)?$')


def program(i: int) -> dict:
    return {'image': bytes(80) + i.to_bytes(8, 'little'), 'data': b'', 'cbufs': []}


class PlaceGraphics(unittest.TestCase):
    def test_placing_many_programs_is_linear(self):
        programs = {f'{i:016x}': program(i) for i in range(COUNT)}
        heap, prog_index, prog_recs = bytearray(), {}, []
        start = time.monotonic()
        for pid in programs:
            packfile.place_graphics(pid, programs, heap, prog_index, prog_recs)
        took = time.monotonic() - start
        self.assertEqual(len(prog_recs), COUNT)
        self.assertLess(took, CEILING_S, f'{COUNT} programs placed in {took:.1f} s')

    def test_a_program_of_a_placed_storage_shares_the_first_records_slot(self):
        programs = {
            'a': program(1),
            'b': dict(program(2), storage_id='a'),
            'c': dict(program(3), storage_id='a'),
        }
        heap, prog_index, prog_recs = bytearray(), {}, []
        for pid in ('a', 'b', 'c'):
            packfile.place_graphics(pid, programs, heap, prog_index, prog_recs)
        self.assertEqual([r['id'] for r in prog_recs], ['a', 'b', 'c'])
        self.assertEqual({r['heap_offset'] for r in prog_recs}, {0})
        self.assertEqual(len(heap), prog_recs[0]['bytes'])


class ProgramIds(unittest.TestCase):
    def test_a_programs_id_is_its_fnv_and_hashed_once_however_often_it_is_asked(self):
        program = {'image': bytes(range(256)) * 16, 'data': b'\1' * 256, 'cbufs': [1, 2]}
        named = program['image'] + program['data'] + bytes(program['cbufs'])
        self.assertEqual(directlink.program_id(program), f'{packfile.fnv(named):x}')
        start = time.monotonic()
        for _ in range(COUNT // 2):
            directlink.program_id(program)
        took = time.monotonic() - start
        self.assertLess(took, CEILING_S, f'{COUNT // 2} ids of one 4.4 KB program in {took:.1f} s')


class LayOutPrograms(unittest.TestCase):
    def test_programs_are_laid_out_once_in_first_use_order_in_linear_time(self):
        programs = {f'{i:016x}': program(i) for i in range(COUNT)}
        ids = list(programs)
        simple = [{'records': [{'prog': {1: (ids[0],), 5: (ids[i],)}}]} for i in range(COUNT)]
        start = time.monotonic()
        heap, prog_index, prog_recs = mkpack.lay_out_programs(simple, programs, [])
        took = time.monotonic() - start
        self.assertEqual([r['id'] for r in prog_recs], ids)
        self.assertLess(took, CEILING_S, f'{COUNT} states laid out in {took:.1f} s')


class PackSerialisation(unittest.TestCase):
    def test_variant_name_matches_regex_and_stays_constant_time(self):
        cases = [
            '',
            'samples=1 | v foo | f bar',
            'samples=1 | v foo | f bar | cubic 0:1 2:3',
            'samples=1 | v foo | f bar | tests=12',
            'samples=1 | v foo | f bar | xrs=3',
            'samples=1 | v foo | f bar | wtex=0:80,1:115',
            'samples=1 | v foo | f bar | cubic a b | tests=1 | xrs=2 | wtex=0:80',
            'samples=1 | v foo | f bar | cubic a b | xrs=2',
            'samples=1 | v foo | f bar | tests=1 | wtex=0:80',
            'samples=1 | v foo | f bar | xrs=2 | tests=1',
            'samples=1 | v foo | f bar | wtex=',
            'samples=1 | v foo | f bar | xrs=',
            'samples=1 | v foo | f bar | tests=',
            'samples=1 | v foo | f bar | cubic ',
            'samples=1 | v foo | f bar | cubic a|b',
            'samples=1 | v foo | f bar | cubic a|b | tests=7',
            'samples=1 | v foo | f bar | tests=\u00b2',
            'samples=1 | v foo | f bar | wtex=0:80,abc',
            'samples=1 | v foo | f bar | l CoreImage | wtex=0:80',
        ]
        for key in cases:
            self.assertEqual(packfile.variant_name(key), VARIANT_NAME_RE.search(key).group(0), key)
        long_key = 'samples=1 | v a ' + ' '.join(f'fc_c{i}:49:{i:02x}' for i in range(60)) + ' | f b | tests=3'
        start = time.monotonic()
        for _ in range(COUNT):
            self.assertEqual(packfile.variant_name(long_key), ' | tests=3')
        took = time.monotonic() - start
        self.assertLess(took, CEILING_S, f'{COUNT} variant_name calls took {took:.2f} s')

    def test_write_json_matches_stdlib_json_dump_byte_for_byte(self):
        class Custom:
            def __str__(self) -> str:
                return 'custom"line\nval'

        pipes = [
            {
                'key': f'samples=1 | v a | f b_{i}\n"\\',
                'blob': [(0x1200, i), (0x1204, i + 1)],
                'empty_list': [],
                'empty_dict': {},
                'mixed': [True, False, None, 1.5, 'a"b\\c\n\u2603', b'\x00\xab', bytearray(b'\xff'), Custom()],
            }
            for i in range(600)
        ]
        pack = packfile.Pack(
            pipes=pipes,
            programs=[{'id': 'abc', 'cbufs': [1, 3]}],
            constexpr_tsc=[[1, 2, 3, 4, 5, 6, 7, 8]],
            baseline={0x100: 1, 0x200: 2},
            init=[(0, 0x100, 1, 0)],
            heap=bytearray(16),
            kernels=[],
            splits=[],
        )
        varying = [0x1200, 0x1204]
        corpus = packfile.fnv(''.join(sorted(r['key'] for r in pack.pipes)).encode())
        js = {
            'version': packfile.VERSION,
            'corpus_hash': f'{corpus:x}',
            'programs': pack.programs,
            'pipelines': pack.pipes,
            'constexpr_tsc': pack.constexpr_tsc,
            'baseline': {f'{m:x}': v for m, v in sorted(pack.baseline.items())},
            'varying': [f'{m:x}' for m in varying],
            'init_methods': len(pack.init),
            'kernels': pack.kernels,
            'splits': pack.splits,
        }
        want = json.dumps(js, indent=1, default=packfile._json_default)
        with tempfile.TemporaryDirectory() as td:
            path = os.path.join(td, 'pack.json')
            packfile.write_json(path, pack, varying)
            with open(path) as f:
                self.assertEqual(f.read(), want)

    def test_pack_bytes_and_write_json_stay_fast(self):
        n = 4_000
        long_consts = ' '.join(f'fc_k{i:02d}:49:{i:02x}' for i in range(40))
        programs = [
            {
                'id': f'{i:016x}',
                'storage_id': f'{i:016x}',
                'name': f'{i:016x}',
                'heap_offset': i * 256,
                'program': i * 256 + 0x30,
                'bytes': 256,
                'code': 160,
                'cbufs': [1, 3],
                'data_offset': 0,
                'data_bytes': 0,
                'gprs': 16,
                'bound_textures': 1,
                'far_ubos': [],
                'facts': 0,
            }
            for i in range(512)
        ]
        pipes = [
            {
                'index': i,
                'tier': 0 if i < 500 else 1,
                'key': f'samples=1 c0=80:15:0:1:0:0:1:0:0 | v fv {long_consts} | f ff {long_consts} {i}',
                'form': '',
                'vs': i % 512,
                'fs': (i * 3) % 512,
                'blob': [(0x1200 + 4 * k, i + k) for k in range(12)],
                'restore': [],
                'tess': [0, 0],
                'push': [(0x704, i)],
                'set_size': 256,
                'entries': [{'off': 0, 'kind': 'ubo', 'stage': 'v', 'slot': 0, 'ssbo': False, 'window': 256}],
                'vbs': [{'stream': 0, 'slot': 0, 'stride': 48}],
                'fetch': False,
                'written_format': [80] + [0] * 15,
                'variants': [],
            }
            for i in range(n)
        ]
        pipes.sort(key=lambda r: (r['tier'], packfile.key_hash(r)))
        pack = packfile.Pack(pipes, programs, [], {0x100: 1}, [], bytearray(256), [])
        start = time.monotonic()
        data, _blob = packfile.pack_bytes(pack)
        with tempfile.TemporaryDirectory() as td:
            path = os.path.join(td, 'pack.json')
            packfile.write_json(path, pack, [0x1200])
            with open(path) as f:
                written = f.read()
        took = time.monotonic() - start
        self.assertGreater(len(data), 0)
        self.assertIn('"corpus_hash":', written)
        self.assertLess(took, CEILING_S, f'{n} pipelines pack_bytes + write_json took {took:.2f} s')

    def test_split_registers_stays_fast_and_exact(self):
        post = {0: {0x0400 + 4 * k: k for k in range(80)}}
        simple = [
            {
                'targets': 3 if i % 2 else 7,
                'records': [
                    {
                        'regs': {0x0400 + 4 * k: (i if k == 3 else k + (1 if k == 5 else 0)) for k in range(80)}
                        | {0x1E40: (i % 2) + 1}
                    }
                ],
            }
            for i in range(COUNT // 4)
        ]
        start = time.monotonic()
        varying, baseline = mkpack.split_registers(simple, post)
        took = time.monotonic() - start
        self.assertEqual(varying, [0x040C])
        self.assertEqual(baseline, {0x0414: 6, 0x1E40: 1})
        self.assertLess(took, CEILING_S, f'split_registers took {took:.2f} s')

    def test_split_identities_evicts_replaced_program_ids(self):
        states = []
        for i in range(64):
            shared_fs = {'image': bytes(80) + b'\x01' * 64, 'data': b'', 'cbufs': [1], 'far_ubos': []}
            split_fs = {'image': bytes(80) + bytes([i]) * 64, 'data': b'', 'cbufs': [1], 'far_ubos': []}
            prologue = {'image': bytes(80) + bytes([i ^ 0x55]) * 64, 'data': b'', 'cbufs': [], 'far_ubos': []}
            states.append(
                {
                    'programs': [
                        {'image': bytes(80) + b'\x02' * 32, 'data': b'', 'cbufs': [], 'far_ubos': []},
                        shared_fs,
                    ],
                    'split': {'record': bytes([i]) * 16, 'programs': [split_fs, prologue]},
                }
            )
        pid_cache: dict[int, str] = {}
        directlink.split_identities(states, pid_cache)
        for rec in states:
            self.assertEqual(directlink._pid(rec['programs'][1], pid_cache), directlink.program_id(rec['programs'][1]))


class Tier0Segments(unittest.TestCase):
    def _make_prog(
        self,
        heap: bytearray,
        *,
        data_bytes: int = 0,
        slm_per_thread: int = 0,
        crs: int = 0,
        gprs: int = 16,
        bound_textures: int = 0,
    ) -> dict:
        off = len(heap)
        slot = bytearray(0x100 + data_bytes)
        # SPH at +0x30: word 1 = slm_per_thread (24-bit), word 3 = crs (24-bit)
        slot[0x34:0x38] = slm_per_thread.to_bytes(4, 'little')
        slot[0x3C:0x40] = crs.to_bytes(4, 'little')
        heap.extend(slot)
        cbufs = [1, 2] if data_bytes else [1, 3]
        return {
            'heap_offset': off,
            'program': off + 0x30,
            'bytes': len(slot),
            'data_offset': 0x100 if data_bytes else 0,
            'data_bytes': data_bytes,
            'cbufs': cbufs,
            'gprs': gprs,
            'bound_textures': bound_textures,
            'far_ubos': [],
            'facts': 0,
        }

    def test_build_tier0_segments_scales_linearly_and_preserves_tier1_offsets(self):
        heap = bytearray()
        programs = [
            self._make_prog(heap, data_bytes=0x100, slm_per_thread=64, crs=512),
            self._make_prog(heap, data_bytes=0, slm_per_thread=0, crs=0, bound_textures=2),
            self._make_prog(heap, data_bytes=0, slm_per_thread=32, crs=0),
        ]
        # Compute program with large QMD/SPH-like bytes outside graphics programs must not inflate tier0_slm_per_warp.
        comp_off = len(heap)
        comp_slot = bytearray(0x100)
        comp_slot[0x34:0x38] = (4096).to_bytes(4, 'little')
        heap.extend(comp_slot)
        programs.append(
            {
                'heap_offset': comp_off,
                'program': comp_off + 0x30,
                'bytes': len(comp_slot),
                'data_offset': 0,
                'data_bytes': 0,
                'cbufs': [1],
                'gprs': 0,
                'bound_textures': 0,
                'far_ubos': [],
                'facts': 0,
            }
        )
        tier0_progs = len(programs)
        # Pad tier 0 heap to TIER_ALIGN so tier-1 starts on a clean boundary; make segments overflow TIER_ALIGN to test delta > 0.
        heap.extend(bytes(packfile.TIER_ALIGN - len(heap)))
        tier0_heap_before = len(heap)
        tier1_marker = bytes((i * 13 + 7) & 0xFF for i in range(0x200))
        t1_off = len(heap)
        heap.extend(tier1_marker)
        programs.append(
            {
                'heap_offset': t1_off,
                'program': t1_off + 0x30,
                'bytes': len(tier1_marker),
                'data_offset': 0,
                'data_bytes': 0,
                'cbufs': [1],
                'gprs': 12,
                'bound_textures': 0,
                'far_ubos': [],
                'facts': 0,
            }
        )
        # Tier-1 program sharing a tier-0 storage_id keeps its tier-0 heap slot when tier 0 grows.
        shared_t0_off = programs[1]['heap_offset']
        programs.append(dict(programs[1]))
        shared_t1_idx = len(programs) - 1

        # 3,500 distinct tier-0 pipelines + duplicates so segments exceed 128 KiB and exercise delta > 0 and deduplication.
        n_tier0 = 3_500
        pipes = []
        for i in range(n_tier0):
            unique = i % 2500
            blob = [
                (0x1A00, 0x1111),
                (0x1200, unique),
                (0x1204, unique + 1),
                (0x1208, unique + 2),
                (0x120C, unique + 3),
            ]
            if i == 1:
                blob.append((0x0DC0, 1))  # PASS_WRITE_METHODS: SET_POLY_OFFSET_POINT
            pipes.append(
                {
                    'tier': 0,
                    'key': f'samples=1 c0=80:15:0:1:0:0:1:0:0 | v fv | f ff_{i}',
                    'vs': 0 if i % 2 == 0 else 2,
                    'fs': 1,
                    'blob': blob,
                    'entries': [],
                    'push': [],
                    'vbs': [],
                    'set_size': 0,
                    'fetch': False,
                    'variants': [{'stage': 1, 'assume': 1, 'program': 2}] if i == 0 else [],
                }
            )
        pipes.append(
            {
                'tier': 1,
                'key': 'samples=1 c0=80:15:0:1:0:0:1:0:0 | v fv_cold | f ff_cold',
                'vs': tier0_progs,
                'fs': tier0_progs,
                'blob': [(0x1A00, 0x1111)],
                'entries': [],
                'push': [],
                'vbs': [],
                'set_size': 0,
                'fetch': False,
            }
        )
        pipes.sort(key=lambda r: (r['tier'], packfile.key_hash(r)))
        pack = packfile.Pack(
            pipes=pipes,
            programs=programs,
            constexpr_tsc=[],
            baseline={0x1A00: 0x1111},
            init=[],
            heap=heap,
            kernels=[],
            tier0_programs=tier0_progs,
            tier0_heap=tier0_heap_before,
        )

        start = time.monotonic()
        segments.build_tier0_segments(pack)
        data, _blob = packfile.pack_bytes(pack)
        took = time.monotonic() - start

        self.assertLess(took, CEILING_S, f'{n_tier0} tier-0 segments + pack_bytes took {took:.2f} s')
        self.assertGreater(len(data), 0)
        self.assertEqual(pack.tier0_slm_per_warp, 64 * 32 + 512)
        self.assertGreater(pack.tier0_heap, tier0_heap_before)
        self.assertEqual(pack.tier0_heap % packfile.TIER_ALIGN, 0)
        delta = pack.tier0_heap - tier0_heap_before
        self.assertEqual(programs[tier0_progs]['heap_offset'], t1_off + delta)
        self.assertEqual(programs[tier0_progs]['program'], t1_off + delta + 0x30)
        self.assertEqual(programs[shared_t1_idx]['heap_offset'], shared_t0_off)
        self.assertEqual(programs[shared_t1_idx]['program'], shared_t0_off + 0x30)
        self.assertEqual(bytes(pack.heap[t1_off + delta : t1_off + delta + len(tier1_marker)]), tier1_marker)

        pipe_count, combo_count, words_at, words_bytes = segments._SEGMENT_TABLE.unpack_from(pack.segments, 0)
        self.assertEqual(pipe_count, n_tier0)
        self.assertEqual(combo_count, 1)
        distinct_offsets = set()
        reselect_seen = False
        not_kept_seen = False
        kept_seen = False
        for i in range(pipe_count):
            off, words, flags = segments._SEGMENT_ENTRY.unpack_from(
                pack.segments, segments._SEGMENT_TABLE.size + segments._SEGMENT_ENTRY.size * i
            )
            self.assertGreater(words, segments.SEGMENT_BIND_WORDS)
            self.assertTrue(words_at <= off and off + words * 4 <= words_at + words_bytes)
            self.assertEqual(off & 15, 0)
            self.assertLessEqual(off + words * 4, pack.tier0_heap)
            distinct_offsets.add(off)
            reselect_seen |= bool(flags & segments.FLAG_RESELECT_ROOT)
            if flags & segments.FLAG_KEPT_ACROSS_PASSES:
                kept_seen = True
            else:
                not_kept_seen = True
        self.assertLess(len(distinct_offsets), n_tier0)
        self.assertTrue(reselect_seen)
        self.assertTrue(kept_seen)
        self.assertTrue(not_kept_seen)


if __name__ == '__main__':
    unittest.main()
