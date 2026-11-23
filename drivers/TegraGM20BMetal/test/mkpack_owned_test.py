#!/usr/bin/env python3
"""mkpack's per-pipeline and baseline registers never hold a method the driver owns, nor a target a state lacks.

A probe's draws leave the shader local memory area (SET_SHADER_LOCAL_MEMORY_A..E) its own process sized, at its own
address: a pack carrying it made raw bind another process's GPU address over the area slm.h gives its channel.
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'pack'))
import directlink
import mkpack
import packfile

# what iOS 27's probe left, from the pack's report
PROBE_SLM = {0x0790: 0x3E, 0x0794: 0xFBF6C000, 0x079C: 0x60000, 0x07A0: 0xC00}
PROGRAMS = [{'image': bytes([i]), 'data': b'', 'cbufs': []} for i in range(2)]


def state(regs: dict[int, int], key: str = 'samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0') -> dict:
    return {'key': key, 'targets': mkpack.targets(key), 'records': [{'regs': regs}]}


def made(key: str, regs: dict[int, int], raster_ordered: bool = False) -> dict:
    """Give a state as directlink.load_records gives directpack's."""
    fields = {'vs': 0, 'fs': 1, 'blob': [], 'entries': [], 'push': [], 'vbs': [], 'set_size': 0, 'fetch': False}
    rec = {'key': key, 'raster_ordered': raster_ordered, **fields}
    pipeline = packfile.pipeline_bytes(rec, 0, 0)
    return {'key': key, 'form': '', 'pipeline': pipeline, 'regs': regs, 'programs': PROGRAMS, 'samplers': []}


class OwnedTest(unittest.TestCase):
    def test_shader_local_memory_is_the_runtimes(self) -> None:
        for m in range(0x0790, 0x07A4, 4):
            self.assertTrue(mkpack.owned(m), hex(m))
        self.assertFalse(mkpack.owned(0x077C))  # the window: NVK's context init sets it, the same in every process
        self.assertFalse(mkpack.owned(0x07A4))

    def test_no_blob_or_baseline_takes_the_probes_area(self) -> None:
        simple = [
            state({**PROBE_SLM, 0x1234: 1}),
            state({0x1234: 2}),
            state({**PROBE_SLM, 0x0794: 0xFBF70000, 0x1234: 1}),
        ]
        varying, baseline = mkpack.split_registers(simple, {0: {}})
        self.assertEqual(varying, [0x1234])
        self.assertEqual(baseline, {})


class ForeignTest(unittest.TestCase):
    def test_a_state_without_a_target_leaves_that_targets_registers_in_the_baseline(self) -> None:
        # target 0's SET_BLEND_PER_TARGET_SEPARATE_FOR_ALPHA: the context's 1 under every state with a target 0; a
        # depth-only state's records hold none of a target's registers (directlink.gather drops them)
        simple = [state({0x1E00: 1, 0x1234: 1}), state({0x1E00: 1, 0x1234: 2}), state({}, 'ds=260/0')]
        varying, baseline = mkpack.split_registers(simple, {0: {}})
        self.assertEqual(varying, [0x1234])
        self.assertEqual(baseline, {0x1E00: 1})


class TargetsTest(unittest.TestCase):
    def test_a_record_holds_its_own_targets_registers_and_leaves_separate_alpha_to_the_baseline(self) -> None:
        # the draw's context holds target 0's separate alpha (draw_context.txt), which no state's own registers set;
        # a register of a target the key does not name (0x1A08) is an earlier draw's, not the record's
        context = {0x1E00: 1}
        states = [
            made('samples=1 c0=80:15:0 c1=80:15:0', {0x1A00: 0xF, 0x1A04: 0xF}),
            made('samples=1 c0=80:15:0 c1=80:15:0', {0x1A00: 0x7, 0x1A04: 0x1, 0x1A08: 0x1}),
        ]
        probes = [directlink.probe_of(rec, i, context) for i, rec in enumerate(states)]
        varying, baseline = mkpack.split_registers(probes, {0: {}})
        self.assertEqual([m for m in varying if m not in directlink.PROGRAM_OFFSETS], [0x1A00, 0x1A04])
        self.assertEqual(baseline.get(0x1E00), 1)
        ids = {directlink.program_id(p): i for i, p in enumerate(PROGRAMS)}
        second = directlink.pipeline_of({**states[1], 'probe': probes[1]}, varying, ids, [])
        self.assertEqual(second['blob'], [(0x1A00, 0x7), (0x1A04, 0x1)])


class RasterOrderedTest(unittest.TestCase):
    def test_a_raster_ordered_record_keeps_its_interlock_control_and_its_flag(self) -> None:
        # gpu.gm20b.raster_order: its program runs in the pixel interlock with no storage colour
        # (PipelineRasterOrdered), whose SET_PIXEL_SHADER_INTERLOCK_CONTROL the runtime takes from the record's blob
        # alone
        states = [
            made('samples=1 c0=80:15:0', {0x1A00: 0xF}),
            made('samples=1 c0=70:0:0', {0x1A00: 0x0, 0x1224: 2}, raster_ordered=True),
        ]
        probes = [directlink.probe_of(rec, i, {}) for i, rec in enumerate(states)]
        varying = [0x1A00]
        ids = {directlink.program_id(p): i for i, p in enumerate(PROGRAMS)}
        ordered = directlink.pipeline_of({**states[1], 'probe': probes[1]}, varying, ids, [])
        plain = directlink.pipeline_of({**states[0], 'probe': probes[0]}, varying, ids, [])
        self.assertTrue(ordered['raster_ordered'])
        self.assertIn((0x1224, 2), ordered['blob'])
        self.assertFalse(plain['raster_ordered'])
        self.assertNotIn(0x1224, [m for m, _ in plain['blob']])
        self.assertEqual(directlink.PIPELINE.unpack_from(states[1]['pipeline'])[-1] & 256, 256)


if __name__ == '__main__':
    unittest.main()
