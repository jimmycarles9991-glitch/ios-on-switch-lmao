#!/usr/bin/env python3
"""rawpack.py's split: every bucket starts with the warm-up state, and compute dispatch lines go to a bucket too.

probe.mm draws a bucket's first downsample_4_frag state before the rest, clearing every private target the states load
and giving the dummy sampler NVK's index 0; barrier_set takes a bucket's first line for that warm-up state. A bucket
without it sets aside the first state to load each target as "N draws".
"""

import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'raw', 'pack'))
import probelog
import rawpack

TAIL = (
    ' samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0 c1=80:0:0:1:0:0:1:0:0'
    ' a0=31:0:1:48:1:1 a1=29:16:1:48:1:1 a2=29:24:1:48:1:1 a3=27:32:1:48:1:1'
)
WARM = (
    f'R ok=1 label=com.apple.coreanimation.PBGRAXm_Tds4A2Xhf_Isrc{TAIL}'
    ' | v downsample_vert  | f downsample_4_frag  | \n'
)
DISPATCH = 'D ok=1 key=compute_sum_luma tg=64,1,1 tgmem= tex= smp=\n'


def state(i: int) -> str:
    return f'R ok=1 label=com.apple.coreanimation.P{i}{TAIL} | v fixed_vertex  | f fixed_{i}_frag  | \n'


class SplitTest(unittest.TestCase):
    def split(self, corpus: str) -> list[str]:
        with tempfile.TemporaryDirectory() as scratch:
            path = Path(scratch) / 'corpus.log'
            path.write_text(corpus)
            buckets = rawpack.split_buckets(path, Path(scratch) / 'work', 16)
            return [(d / 'corpus.log').read_text() for d in buckets]

    def test_every_bucket_starts_with_the_warm_up_state(self):
        corpus = DISPATCH + ''.join(state(i) for i in range(64)) + WARM + ''.join(state(i) for i in range(64, 128))
        for text in self.split(corpus):
            self.assertEqual(text.splitlines(keepends=True)[0], WARM)

    def test_dispatch_lines_reach_one_bucket(self):
        corpus = WARM + DISPATCH + ''.join(state(i) for i in range(32))
        self.assertEqual(sum(text.count(DISPATCH) for text in self.split(corpus)), 1)

    def test_an_apps_state_reaches_one_bucket(self):
        app = ' lib=/Applications/SwitchHWTest.app/default.metallib '
        line = f'R ok=1 label=- samples=2 topo=0 ds=252/0 | v quad_vertex{app} | f solid_fragment{app} | \n'
        self.assertEqual(sum(text.count(line) for text in self.split(WARM + line)), 1)


RB = ' lib=/System/Library/PrivateFrameworks/RenderBox.framework/default.metallib'
ICONS = ' lib=/System/Library/PrivateFrameworks/IconRendering.framework/default.metallib'
LINKED = (
    'R ok=1 label=custom_effect:clampToEdges_v1 samples=1 topo=0 ds=260/260 c0=550:15:1:1:5:0:1:5:0'
    f' | v custom_effect_vertex{RB}  | f custom_effect_fragment{RB} #0:36:6504'
    f' | fp clampToEdges_v1{ICONS} as=custom_fn #1:33:00000000 | '
)


class StateKeyTest(unittest.TestCase):
    def test_a_linked_function_is_keyed_without_its_library(self):
        # RawMetalLibrary.mm raw_metal_pipeline_key: the stages, the linked functions, then the stages' framework
        self.assertEqual(
            rawpack.state_key(LINKED),
            'samples=1 topo=0 ds=260/260 c0=550:15:1:1:5:0:1:5:0  | v custom_effect_vertex  | f custom_effect_fragment'
            ' #0:36:6504 | fp clampToEdges_v1 as=custom_fn #1:33:00000000 | l RenderBox',
        )

    def test_a_fragment_with_no_constants_keeps_its_space_before_the_framework(self):
        # MapKit.SnapshotService's VectorKit state as the device logged its key (a state the pack lacks made, 24B119)
        vectorkit = ' lib=/System/Library/PrivateFrameworks/VectorKit.framework/default.metallib'
        line = (
            'R ok=1 label=- samples=1 topo=0 ds=0/0 c0=81:15:0:1:0:0:1:0:0 a0=29:0:0:16:1:1 a1=29:8:0:16:1:1'
            f' | v ImageCopy::imageCopy_vertex{vectorkit}  | f ImageCopy::imageCopy_fragment{vectorkit}  | '
        )
        self.assertEqual(
            rawpack.state_key(line),
            'samples=1 topo=0 ds=0/0 c0=81:15:0:1:0:0:1:0:0 a0=29:0:0:16:1:1 a1=29:8:0:16:1:1'
            '  | v ImageCopy::imageCopy_vertex  | f ImageCopy::imageCopy_fragment  | l VectorKit',
        )
        quartzcore = line.replace(vectorkit, '')
        self.assertTrue(rawpack.state_key(quartzcore).endswith(' | f ImageCopy::imageCopy_fragment'))

    def test_a_cubic_variant_is_keyed_after_its_states_framework(self):
        # raw_cubic.h CubicBinds::key after the state's key, as RawMetalCubic.mm names the variant
        line = LINKED + 'cubic s0:0000n  t0:80 | '
        self.assertEqual(rawpack.state_key(line), rawpack.state_key(LINKED) + ' | cubic s0:0000n t0:80')

    def test_a_written_variant_is_keyed_last(self):
        # raw_written_textures.h writtenVariant after the sampled two-plane slots, as RawMetalCubic.mm names the variant
        line = LINKED + 'wtex=1:23 | xrs=2 | '
        self.assertEqual(rawpack.state_key(line), rawpack.state_key(LINKED) + ' | xrs=2 | wtex=1:23')


class CoreAnimationTest(unittest.TestCase):
    def test_a_harvested_sampled_two_plane_line_is_kept_as_drawn(self):
        # tools/gpu/logharvest.py's line of a Core Animation state's " | xrs=" variant: its texture is at slot 3
        line = (
            'R ok=1 label=com.apple.coreanimation.Pb3a8Xm_TlubA2Xhf_Isrc samples=1 topo=0 ds=0/0'
            ' c0=550:15:0:1:0:0:1:0:0 c1=550:0:0:1:0:0:1:0:0 a0=31:0:1:48:1:1 a1=29:16:1:48:1:1 a2=29:24:1:48:1:1'
            ' a3=27:32:1:48:1:1 | v std_vert1_lph  | f luma_boost_lph  | '
        )
        tables = Path(rawpack.HERE) / 'ca' / 'qc-24B119.json'
        with tempfile.TemporaryDirectory() as scratch:
            corpus = Path(scratch) / 'corpus.log'
            corpus.write_text(f'{line}\n{line}xrs=8 | \n')
            harvested = dict(rawpack.ca_states(tables, corpus))['harvested']
        state = rawpack.state_key(line)
        self.assertEqual([rawpack.state_key(drawn) for drawn in harvested], [state, state + ' | xrs=8'])


class ProbeLogTest(unittest.TestCase):
    def test_an_empty_submission_is_no_probes(self):
        with tempfile.TemporaryDirectory() as scratch:
            log = Path(scratch) / 'probe.log'
            log.write_text('S regs 0\nSUBMIT 1\nSUBMIT 2\nDRAW 4\nSUBMIT 3\n')
            self.assertEqual(probelog._submissions(str(log)), [['S regs 0', 'SUBMIT 1'], ['DRAW 4', 'SUBMIT 3']])


if __name__ == '__main__':
    unittest.main()
