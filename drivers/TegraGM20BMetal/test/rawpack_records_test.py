#!/usr/bin/env python3
"""What the raw pack generator keeps of a probe.

A texture3d argument is an entry with no slot, a state holds up to 32 set-0 entries, the buffer m2v binds of its own
beside an 8-bit sRGB target is named as such, and a storage buffer's SlotBindOffset residual word is named by its code
(raw/pack/probe_bind.h, the runtime's raw_residual_test), a compute kernel's from probecompute.mm's bind-offset
dispatch.
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


def kernel(entries: list[dict], push: dict[int, int], prog: str = 'p') -> dict:
    return {'prog': prog, 'qmd': [0] * 64, 'region': 4, 'set_bytes': 0x40, 'push': push, 'entries': entries}


def buffer(off: int, slot: int, window: int = 0) -> dict:
    return {'off': off, 'kind': kernels.BUFFER, 'slot': slot, 'window': window}


class KernelResidualsTest(unittest.TestCase):
    def test_codes_are_storage_residuals_distinct_below_fifteen(self):
        codes = [kernels.residual_code(slot) for slot in range(31)]
        self.assertEqual(len(set(codes[:15])), 15)
        self.assertTrue(all(0 < c < 16 for c in codes))
        self.assertEqual(kernels.residual_code(15), kernels.residual_code(0))

    def test_each_buffer_takes_the_word_set_to_its_code(self):
        plain = kernel([buffer(0, 0), buffer(16, 2, 64)], {0: 0x11})
        offset = kernel(
            [buffer(0, 0, 32), buffer(16, 2, 80)], {0: 0x11, 1: kernels.residual_code(0), 3: kernels.residual_code(2)}
        )
        kernels.attach_residuals(plain, offset)
        self.assertEqual([e['residual'] for e in plain['entries']], [1, 3])
        self.assertEqual(packfile.kernel_window(plain['entries'][0]), 2 << 24)
        self.assertEqual(packfile.kernel_window(plain['entries'][1]), 64 | 4 << 24)

    def test_a_word_set_before_or_twice_names_no_slot(self):
        plain = kernel([buffer(0, 0), buffer(16, 1)], {1: 5})
        offset = kernel(
            [buffer(0, 0), buffer(16, 1)],
            {1: kernels.residual_code(0), 2: kernels.residual_code(1), 3: kernels.residual_code(1)},
        )
        kernels.attach_residuals(plain, offset)
        self.assertEqual([e['residual'] for e in plain['entries']], [None, None])
        self.assertEqual(packfile.kernel_window(plain['entries'][0]), 0)

    def test_slots_sharing_a_code_name_no_word(self):
        plain = kernel([buffer(0, 0), buffer(16, 15)], {})
        offset = kernel([buffer(0, 0), buffer(16, 15)], {2: kernels.residual_code(0)})
        kernels.attach_residuals(plain, offset)
        self.assertEqual([e['residual'] for e in plain['entries']], [None, None])

    def test_a_kernel_without_its_bind_offset_dispatch_keeps_its_record_with_no_words(self):
        plain = kernel([buffer(0, 0, 32)], {})
        kernels.attach_residuals(plain, None)
        self.assertIsNone(plain['entries'][0]['residual'])
        self.assertEqual(packfile.kernel_window(plain['entries'][0]), 32)

    def test_a_program_that_moves_with_the_offsets_takes_none(self):
        plain = kernel([buffer(0, 0)], {})
        offset = kernel([buffer(0, 0)], {2: kernels.residual_code(0)}, prog='q')
        kernels.attach_residuals(plain, offset)
        self.assertIsNone(plain['entries'][0]['residual'])


if __name__ == '__main__':
    unittest.main()
