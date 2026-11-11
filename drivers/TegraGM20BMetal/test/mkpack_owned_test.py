#!/usr/bin/env python3
"""mkpack's per-pipeline and baseline registers never hold a method the raw path owns.

A probe's draws leave the shader local memory area (SET_SHADER_LOCAL_MEMORY_A..E) its own process sized, at its own
address: a pack carrying it made raw bind another process's GPU address over the area raw_slm.h gives its channel.
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'raw', 'pack'))
import mkpack

# what iOS 27's probe left, from the pack's report
PROBE_SLM = {0x0790: 0x3E, 0x0794: 0xFBF6C000, 0x079C: 0x60000, 0x07A0: 0xC00}


def state(regs: dict[int, int], key: str = 'samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0') -> dict:
    return {'key': key, 'records': [{'regs': regs}]}


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


if __name__ == '__main__':
    unittest.main()
