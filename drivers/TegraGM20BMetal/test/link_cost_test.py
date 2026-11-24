#!/usr/bin/env python3
"""The pack's link step stays linear in programs and states (pack/packfile.py place_graphics, pack/mkpack.py
lay_out_programs) and hashes each program once (pack/directlink.py program_id): the closed Core Animation set makes
the link place well over 100,000 programs and name them over a million times.

Each case does at 40,000 what a link does: a scan of every placed record per new program took about 14 s here at
this size and grows with its square; a probe takes milliseconds. The ceiling leaves a remote worker 20 times the
linear time and is far below the quadratic one.
"""

import os
import sys
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'pack'))
import directlink  # noqa: E402
import mkpack  # noqa: E402
import packfile  # noqa: E402

COUNT = 40_000
CEILING_S = 3.0


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


if __name__ == '__main__':
    unittest.main()
