"""The `gpu` milestone against kernel log lines and model logs of emulator runs."""

import tempfile
import unittest
from pathlib import Path

from tools.emu import gpucheck

BRINGUP = [
    '[     1.248893] tegra-gm20b: M2 ok wpr=[ffd00000-ffd40000] acr=done fecs_mb0=00000000 gpc=1 tpc=2 ret=0',
    '[     1.272311] tegra-gm20b: M3 ok vm=ok chan=ok fence=1/1 ce_fill_bad=0/1024 (L2-cached target) err=0 ret=0',
    '[     1.275228] tegra-gm20b: M4 ok published AGXAccelerator (MetalPluginName TegraGM20BMetal)',
    '[     1.277602] tegra-gm20b: pool scrub: 252 MiB queued to the copy engine 17 ms after M3 (the CPU cleared it)',
]


class GpuMilestoneTest(unittest.TestCase):
    def test_reached_at_the_scrub_once_m3_and_m4_passed(self):
        self.assertIn('pool scrub', gpucheck.reached(BRINGUP))
        self.assertIsNone(gpucheck.failure(BRINGUP))

    def test_not_reached_without_m4(self):
        self.assertIsNone(gpucheck.reached([BRINGUP[0], BRINGUP[1], BRINGUP[3]]))

    def test_reached_once_the_scrub_has_settled_without_its_queued_line(self):
        later = '[     3.633340] SwitchALS: off: BH1730 on I2C2 0x29 never answered'
        self.assertIsNone(gpucheck.reached([*BRINGUP[:3], '[     3.272310] SwitchAudio: headphones in at start']))
        self.assertEqual(gpucheck.reached([*BRINGUP[:3], later]), later)

    def test_bringup_and_gpu_errors_fail(self):
        for line in (
            '[     1.343058] tegra-gm20b: M3 FAIL vm=ok chan=no fence=0/0 ce_fill_bad=0/1024 err=0 ret=-28',
            '[     1.342988] tegra-gm20b 57000000.gpu: fifo:000000: !cgids',
            '[     3.300215] tegra-gm20b WARN /tree/drivers/TegraGM20B/nouveau/nvkm/engine/gr/ctxgf100.c:1451: timeout',
            (
                '[     9.100000] tegra-gm20b 57000000.gpu: fifo:000000: PBDMA0: 00040000 [PBENTRY] ch 18 '
                '[00e4230000 ce-copy] subc 4 mthd 0300 data 00000386'
            ),
            (
                '[     9.100000] tegra-gm20b 57000000.gpu: fifo:000000: fault 01 [WRITE] at 0000102000 engine 1b '
                '[CE2] client 19 [HUB/CE2] reason 06 [RO_VIOLATION] on channel 3 [00e4231000 scrub]'
            ),
            '[     9.200000] tegra-gm20b: GPU error in scrub (channel 3): channel killed, GPU running: ...',
            '[     9.300000] tegra-gm20b: pool scrub unavailable (-12): pool pages are zeroed by the CPU',
        ):
            with self.subTest(line=line):
                self.assertEqual(gpucheck.failure([*BRINGUP, line]), line)

    def test_a_run_with_no_method_or_a_fault_fails_after_the_run(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            self.assertEqual(gpucheck.after_run(run), ['the GPU executed no method'])
            (run / 'gpu-methods.raw').write_text('0 4 b0b5 0300 00000586\n')
            self.assertEqual(gpucheck.after_run(run), ['the scrubber ran no fill'])
            (run / 'gpu-methods.raw').write_text('0 4 b0b5 0418 00008000\n0 4 b0b5 0300 00000786\n')
            (run / 'gpu-events.log').write_text('5 runlist 0 submit: 1 entries at 0xe42be000\n')
            self.assertEqual(gpucheck.after_run(run), [])
            (run / 'gpu-events.log').write_text('6 channel 0 stopped: pushbuffer fetch\n')
            self.assertEqual(gpucheck.after_run(run), ['6 channel 0 stopped: pushbuffer fetch'])

    def test_a_method_the_class_lacks_fails_after_the_run(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            (run / 'gpu-methods.raw').write_text('0 4 b0b5 0418 00008000\n0 0 b197 3794 00000001\n')
            problems = gpucheck.after_run(run)
            self.assertEqual(len(problems), 1)
            self.assertIn('MAXWELL_B 0x3794 00000001 !NOT_IN_CLASS', problems[0])


if __name__ == '__main__':
    unittest.main()
