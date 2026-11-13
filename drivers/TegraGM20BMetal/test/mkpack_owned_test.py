#!/usr/bin/env python3
"""mkpack's per-pipeline and baseline registers never hold a method the raw path owns, nor a target a state lacks.

A probe's draws leave the shader local memory area (SET_SHADER_LOCAL_MEMORY_A..E) its own process sized, at its own
address: a pack carrying it made raw bind another process's GPU address over the area raw_slm.h gives its channel.

A state's targets are those its draw writes: a two-plane state's RGB10A8_2P_XR10 alpha targets are its own, at the
indices runtime/raw_xr_pass.h gives them.
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'raw', 'pack'))
import directlink
import mkpack
import packfile

# what iOS 27's probe left, from the pack's report
PROBE_SLM = {0x0790: 0x3E, 0x0794: 0xFBF6C000, 0x079C: 0x60000, 0x07A0: 0xC00}
PROGRAMS = [{'image': bytes([i]), 'data': b'', 'cbufs': []} for i in range(2)]


def state(regs: dict[int, int], key: str = 'samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0') -> dict:
    return {'key': key, 'targets': mkpack.targets(key), 'records': [{'regs': regs}]}


def made(key: str, regs: dict[int, int], two_plane: bool) -> dict:
    """Give a state as directlink.load_records gives directpack's."""
    fields = {'vs': 0, 'fs': 1, 'blob': [], 'entries': [], 'push': [], 'vbs': [], 'set_size': 0, 'fetch': False}
    pipeline = packfile.pipeline_bytes({'key': key, 'two_plane': two_plane, **fields}, 0, 0)
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


class TwoPlaneTest(unittest.TestCase):
    def test_the_alpha_index_is_the_lowest_free_one(self) -> None:
        # raw_xr_pass_test's TheAlphaIndexIsTheLowestFreeOne, the runtime's xrAlphaSlots on the same formats
        cases = {
            (550,): [1],
            (550, 550): [2, 3],
            (550, 105, 550): [3, -1, 4],
            (80, 550): [-1, 2],
            (550, 0, 80): [1],
            (550, 80, 80, 80, 80, 80, 80, 80): [],
            (552, 554): [],
        }
        for formats, alpha in cases.items():
            with self.subTest(formats=formats):
                padded = [*formats] + [0] * (8 - len(formats))
                self.assertEqual(mkpack.xr_alpha_slots(padded), alpha + [-1] * (8 - len(alpha)))

    def test_a_two_plane_states_targets_take_each_alpha_target(self) -> None:
        key = 'samples=1 topo=0 ds=0/0 c0=550:15:1:1:5:0:1:5:0 c1=105:0:0:1:0:0:1:0:0 c2=550:15:0  | v a | f b'
        self.assertEqual(mkpack.colour_formats(key), [550, 105, 550, 0, 0, 0, 0, 0])
        self.assertEqual(mkpack.target_mask(key, two_plane=False), 0b111)
        self.assertEqual(mkpack.target_mask(key, two_plane=True), 0b11111)

    def test_a_two_plane_record_writes_its_alpha_target_and_leaves_separate_alpha_to_the_baseline(self) -> None:
        # the draw's context holds target 0's separate alpha (draw_context.txt), which no state's own registers set
        context = {0x1E00: 1}
        states = [
            made('samples=1 c0=80:15:0 c1=80:15:0', {0x1A00: 0xF, 0x1A04: 0xF}, two_plane=False),
            made('samples=1 c0=550:15:0', {0x1A00: 0x7, 0x1A04: 0x1}, two_plane=True),
        ]
        probes = [directlink.probe_of(rec, i, context) for i, rec in enumerate(states)]
        varying, baseline = mkpack.split_registers(probes, {0: {}})
        self.assertEqual([m for m in varying if m not in directlink.PROGRAM_OFFSETS], [0x1A00, 0x1A04])
        self.assertEqual(baseline.get(0x1E00), 1)
        ids = {directlink.program_id(p): i for i, p in enumerate(PROGRAMS)}
        two_plane = directlink.pipeline_of({**states[1], 'probe': probes[1]}, varying, ids, [])
        self.assertEqual(two_plane['blob'], [(0x1A00, 0x7), (0x1A04, 0x1)])


if __name__ == '__main__':
    unittest.main()
