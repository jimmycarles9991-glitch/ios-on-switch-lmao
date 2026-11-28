#!/usr/bin/env python3
"""A pre-built segment's FLAG_BLEND_KILL (pack.h kPackSegmentBlendKill) is its own last SET_ITERATED_BLEND_OPTIMIZATION,
so a state's creation reads the kill from the wired segment table and never from the segment's words in PackHeap.

A fetch-free iterated-blend state's segment writes the method again, NOOP_NEVER, after its blend's own word: the flag
follows the GPU's word there, not the blend.
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'pack'))
import segments  # noqa: E402

KEY = 'samples=1 topo=3 ds=0/0 c0=80:15:1:1:1:0:1:1:0 | v vs | f fs'
BLENDS = [(0x1360, 1)] + [(0x1E04 + 4 * i, v) for i, v in enumerate((0x8006, 0x4001, 0x4001, 0x8006, 0x4001, 0x4001))]
WRITES_TARGET_0 = [0x1111] + [0] * 7


def program(at: int) -> dict:
    return {'program': at, 'gprs': 8, 'cbufs': [1], 'heap_offset': at, 'data_offset': 0}


def stream(blob: list[tuple[int, int]], rec: dict | None = None, iterated: list[dict] | None = None):
    rec = {'key': KEY, 'blob': blob, **(rec or {})}
    return segments.build_pipeline_stream(
        rec, program(0x100), program(0x200), WRITES_TARGET_0, iterated or [], None, frozenset()
    )


class SegmentBlendKill(unittest.TestCase):
    def test_a_zero_source_blend_sets_the_flag(self):
        words, flags = stream(BLENDS)
        self.assertTrue(flags & segments.FLAG_BLEND_KILL)
        self.assertEqual(
            segments._last_written(words, segments.ITERATED_BLEND_OPTIMIZATION, -1), segments.NOOP_SOURCE_RGBA_0000
        )

    def test_a_state_that_does_not_blend_leaves_it_clear(self):
        _, flags = stream([(0x1360, 0)] + BLENDS[1:])
        self.assertFalse(flags & segments.FLAG_BLEND_KILL)

    def test_an_iterated_blend_programs_later_word_clears_it(self):
        iterated = [{'words': [7, 8], 'constants': {}, 'alpha_source': 0x4001, 'alpha_dest': 0x4001, 'enable': 1}]
        words, flags = stream(
            BLENDS, {'form': 'free', 'fetch_rung': [segments.FETCH_ITERATED_BLEND], 'iterated': 1}, iterated
        )
        self.assertFalse(flags & segments.FLAG_BLEND_KILL)
        self.assertEqual(segments._last_written(words, segments.ITERATED_BLEND_OPTIMIZATION, -1), segments.NOOP_NEVER)

    def test_the_last_write_reads_every_header_form(self):
        m = segments.ITERATED_BLEND_OPTIMIZATION
        self.assertEqual(segments._last_written([], m, 9), 9)
        immediate = (4 << 29) | (1 << 16) | (m >> 2)
        self.assertEqual(segments._last_written([immediate], m, 9), 1)
        increasing = (1 << 29) | (2 << 16) | ((m - 4) >> 2)
        self.assertEqual(segments._last_written([increasing, 5, 6], m, 9), 6)
        non_increasing = (3 << 29) | (2 << 16) | (m >> 2)
        self.assertEqual(segments._last_written([non_increasing, 5, 6], m, 9), 6)
        other_subchannel = (4 << 29) | (1 << 16) | (1 << 13) | (m >> 2)
        self.assertEqual(segments._last_written([other_subchannel], m, 9), 9)


if __name__ == '__main__':
    unittest.main()
