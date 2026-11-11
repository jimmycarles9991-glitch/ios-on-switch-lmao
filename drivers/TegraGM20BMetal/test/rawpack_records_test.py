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
import layouts
import mkpack
import packfile


def handle(tex: tuple | None) -> dict:
    return {'off': 228, 'kind': 'handle', 'tex': tex, 'smp': None}


class RecordsTest(unittest.TestCase):
    def test_only_known_own_shader_pairs_use_alias_keys(self):
        own = 'samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0 | v resolve_vert  | f resolve_bgra8_frag'
        self.assertEqual(packfile.key_hash({'key': own}), packfile.fnv(b'raw.resolve.resolve_bgra8_frag'))
        mip = 'samples=1 topo=0 ds=0/0 c0=43:15:0:1:0:0:1:0:0 | v mip_vert  | f mip_bgr5a1_frag'
        self.assertEqual(packfile.key_hash({'key': mip}), packfile.fnv(b'raw.mip.mip_bgr5a1_frag'))
        for key in (
            own + ' | l RenderBox',
            own + ' | air 1234',
            own + ' | cubic 1',
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

    def test_a_texture3d_argument_is_an_entry_with_no_slot(self):
        problems: list[str] = []
        ent = mkpack.handle_entry((228, 'handle', ('tex3d', 'R102'), None, 0), {}, [], problems)
        self.assertEqual(problems, [])
        self.assertEqual(ent['tex'], ('tex3d', 0xFF))
        off, kind, stage, slot = struct.unpack_from('<HBBB', packfile.entry_bytes([ent]))
        self.assertEqual((off, kind, stage, slot), (228, 4, 2, 0xFF))

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
    body = struct.pack('<QIHBb', 0, 0, 0, len(entries), 4) + struct.pack('<64I', *qmd)
    body += struct.pack('<HHI', 0x40, 0, 1 << 2) + struct.pack('<32I', *words)
    body += b''.join(kernels.ENTRY.pack(*e) for e in entries) + bytes(kernels.ENTRY.size * (16 - len(entries)))
    rec = {'outcome': outcome, 'key': key, 'why': 'a texture slot the dispatch leaves unbound at 0' if outcome else ''}
    if not outcome:
        rec.update(kernel=body, program='p', samplers=samplers)
    return rec


class KernelRecordsTest(unittest.TestCase):
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
