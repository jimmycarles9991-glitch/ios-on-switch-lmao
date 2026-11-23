#!/usr/bin/env python3
"""A state of no fragment function is packed: its record names no fragment program (runtime/pack.h kNoProgram).

RenderBox's four path_edges states are the corpus's (precompile/corpus.log): each draws depth and stencil from its
vertex function alone. The run time keys a stage by its library function, never the name it was specialized under
(TegraGM20BMetalLibrary.mm metal_pipeline_key, TegraGM20BMetalCompileRequest.mm requestPackKey; the pack's
probe_states.h alike), so the line naming path_edges_vertex as=vhba14dhipa4 is the state of the line naming it plainly:
three keys.
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'pack'))
import directlink
import make_pack
import mkpack
import packfile

CORPUS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'precompile', 'corpus.log')
RENDER_BOX_KEYS = {0x5A8334BB122A80B3, 0x970DB699817A2E73, 0xE0CA57468521CCEB}
VERTEX = {'image': bytes(80) + b'\x01', 'data': b'', 'cbufs': [1]}
NONE = {'image': b'', 'data': b'', 'cbufs': []}
KEY = 'samples=1 topo=0 ds=260/260 c0=115:0:0:1:0:0:1:0:0  | v path_edges_vertex  | f -  | l RenderBox'


def made(key: str, programs: list[dict]) -> dict:
    """Give a state as directlink.load_records gives directpack's."""
    fields = {'vs': 0, 'fs': 1, 'blob': [], 'entries': [], 'push': [], 'vbs': [], 'set_size': 0, 'fetch': False}
    pipeline = packfile.pipeline_bytes({'key': key, **fields}, 0, 0)
    return {'key': key, 'form': '', 'pipeline': pipeline, 'regs': {0x1A00: 0}, 'programs': programs, 'samplers': []}


class KeyTest(unittest.TestCase):
    def test_render_box_path_edges_states_key_as_the_run_time_keys_them(self) -> None:
        with open(CORPUS) as f:
            lines = [line.rstrip('\n') for line in f if ' | v path_edges_vertex ' in line and ' | f - ' in line]
        self.assertEqual(len(lines), 4)
        self.assertEqual(sum(' as=' in line for line in lines), 1)
        hashes = {packfile.fnv(make_pack.state_key(line).encode()) for line in lines}
        self.assertEqual(hashes, RENDER_BOX_KEYS)

    def test_a_state_metal_does_not_rasterize_keys_rast_0_after_its_attachments(self) -> None:
        line = 'R ok=1 label=x samples=1 topo=0 ds=252/0 rast=0 a0=31:0:0:16:1:1 | v v  | f - '
        self.assertEqual(make_pack.state_key(line), 'samples=1 topo=0 ds=252/0 a0=31:0:0:16:1:1 rast=0  | v v  | f -')
        self.assertNotIn('rast=', make_pack.state_key(line.replace(' rast=0', '')))


class LinkTest(unittest.TestCase):
    def test_a_record_of_no_fragment_program_packs_its_vertex_program_alone(self) -> None:
        states = [made(KEY, [VERTEX, NONE])]
        probes = [directlink.probe_of(rec, i, {}) for i, rec in enumerate(states)]
        self.assertEqual(sorted(probes[0]['records'][0]['prog']), [1])
        self.assertNotIn(directlink.PROGRAM_OFFSETS[1], probes[0]['records'][0]['regs'])
        programs = {directlink.program_id(p): p for rec in states for p in directlink.stage_programs(rec)}
        heap, prog_index, prog_recs = mkpack.lay_out_programs(probes, programs, [])
        self.assertEqual(len(prog_recs), 1)
        varying, _ = mkpack.split_registers(probes, {0: {}})
        pipe = directlink.pipeline_of({**states[0], 'probe': probes[0]}, varying, prog_index, [])
        self.assertEqual(pipe['fs'], packfile.NO_PROGRAM)
        self.assertEqual(pipe['vs'], prog_index[directlink.program_id(VERTEX)])
        head = packfile.pipeline_bytes(dict(pipe, set_size=0), 0, 0)
        self.assertEqual(int.from_bytes(head[16:20], 'little'), 0xFFFFFFFF)
        self.assertGreater(len(heap), 0)

    def test_the_probes_record_of_one_program_is_no_longer_set_aside(self) -> None:
        program = {1: ('a', 0, 'known')}
        self.assertIsNone(mkpack.record_reason([{'prog': program}]))
        self.assertEqual(mkpack.record_reason([{'prog': {5: ('b', 0, 'known')}}]), 'stages [5]')

    def test_the_pixel_stage_and_rasteriser_switches_are_the_runtimes(self) -> None:
        self.assertTrue(mkpack.owned(0x2140))  # SET_PIPELINE_SHADER(PIXEL)
        self.assertTrue(mkpack.owned(0x037C))  # SET_RASTER_ENABLE
        self.assertFalse(mkpack.owned(0x2100))  # SET_PIPELINE_SHADER(GEOMETRY), the context's


if __name__ == '__main__':
    unittest.main()
