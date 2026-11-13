"""tools/frames over lines in the formats the kernel and the GPU user side log."""

import unittest

from tools.frames import frames

KERNEL = (
    'kernel: SwitchFrame: latch 192000000 after 3 frames (50000 us), cause gpu; origin vsync 191040000; '
    'first submit 4000, last submit 6000 (3, exec 300 us), swap 6500, offer 6600, gpu done 31000, '
    'flip 31200, latch 50000 us; other submits 2; swap thread base 47 sched 47 mode 3 qos 6; '
    'cpu 1785 MHz at vsync 1785 at swap; imp 2 340 us, bind 3 90 us, alloc 1 20 us, free 0, clean 7200 KiB 610 us'
)
USER = 'backboardd: SwitchFrameUser: backboardd cb 42 commit 191100000 replay 191100100 191200000 fence 0'
RENDER = (
    'kernel: SwitchFrameRender: vsync 191040000: woke 40 us after it; first submit 4000 us (cpu 3800, blocked 120 us '
    'in 2); swap 6500 us (cpu 6100, blocked 300 us in 3, waiting for a core 20 us); longest block 180 us, user pc dsc '
    '0x1cb780b58 lr dsc 0x1cb784224, kernel continuation 0x0; dropped 0'
)


INTERACTION = (
    '2026-11-06 19:20:47.123 Df kernel.development[0:b9] SwitchFrames interaction: 1310 ms (touch 610 ms), 55 '
    'presents: 1:50 2:1 3:1 4+:1, 7 periods lost, worst 300.0 ms, still 1; long idle 0 ca 1 gpu 1 queue 0 latch 0 '
    'rate 1; SpringBoard[27] 60 commits <1:0 1:55 2:3 3:1 4+:0; delivery 110/112 reports p50/p99/max 256/2048/2900 us '
    'to SpringBoard[27]'
)

LATENCY = (
    '2026-11-06 19:20:47.124 Df kernel.development[0:b9] SwitchFrames latency: began 3 (dropped 0): total '
    '41.0/52.0/52.0 = deliver 1.0/1.5/1.5 + app 4.0/9.0/9.0 + wait 9.5/15.0/15.0 + compose 4.0/5.0/5.0 + show '
    '14.5/16.0/16.0 ms p50/p95/max'
)


class FramesTest(unittest.TestCase):
    def test_interaction_rows(self):
        rows = frames.interactions([KERNEL, INTERACTION])
        self.assertEqual(len(rows), 2)
        self.assertIn('19:20:47.123  1310 ms (touch   610):   55 presents, 1:50 2:1 3:1 4+:1', rows[0])
        self.assertIn('(93% one frame apart)', rows[0])
        self.assertIn('worst 300.0 ms, still 1; long idle 0 ca 1 gpu 1 queue 0 latch 0 rate 1', rows[0])
        self.assertTrue(rows[1].strip().startswith('SpringBoard[27] 60 commits'))

    def test_latency_follows_its_interaction(self):
        rows = frames.interactions([INTERACTION, LATENCY])
        self.assertEqual(len(rows), 3)
        self.assertEqual(rows[2], '    latency: ' + LATENCY.split('SwitchFrames latency: ')[1])

    def test_legacy_still_intervals_remain_in_the_denominator(self):
        line = INTERACTION.replace('55 presents', '105 presents')
        self.assertIn('(48% one frame apart)', frames.interactions([line])[0])

    def test_stages_and_join(self):
        got = frames.collect([KERNEL, USER])
        self.assertEqual(len(got), 1)
        frame = got[0]
        stages = {name: us for name, us, _ in frame.stages()}
        self.assertEqual(stages['vsync->first submit'], 4000)
        self.assertEqual(stages['offer->gpu done'], 24400)
        self.assertEqual(stages['flip->latch'], 18800)
        self.assertEqual(frame.user[0][2], 42)

    def test_render_thread_joins_by_vsync(self):
        frame = frames.collect([RENDER, KERNEL])[0]
        self.assertTrue(frame.render.startswith('woke 40 us after it; first submit 4000 us (cpu 3800'))
        self.assertIn('  render thread: woke 40 us', '\n'.join(frames.describe(frame)))
        other = RENDER.replace('vsync 191040000', 'vsync 191000000')
        self.assertEqual(frames.collect([other, KERNEL])[0].render, '')

    def test_new_surface_work(self):
        frame = frames.collect([KERNEL])[0]
        self.assertEqual(frame.surfaces['imp'], 2)
        self.assertEqual(frame.surfaces['clean_kib'], 7200)
        self.assertIn('2 imports (340 us, cleaned 7200 KiB in 610 us)', '\n'.join(frames.describe(frame)))

    def test_missing_points_are_skipped(self):
        line = KERNEL.replace('first submit 4000, last submit 6000', 'first submit -1, last submit -1')
        names = [name for name, _, _ in frames.collect([line])[0].stages()]
        self.assertNotIn('vsync->first submit', names)
        self.assertIn('swap->offer', names)

    def test_summary_names_the_costliest_stage(self):
        out = '\n'.join(frames.summary(frames.collect([KERNEL])))
        self.assertIn('costliest stage: offer->gpu done 1', out)
        self.assertIn('2 refresh periods lost', out)


if __name__ == '__main__':
    unittest.main()
