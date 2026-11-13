import tempfile
import unittest
from pathlib import Path

from tools.emu import gpulog

RAW = """\
3 0 b06f 0000 0000b197
3 4 b06f 0000 0000b0b5
3 0 b197 1b00 00000001
3 0 b197 0384 00000000
3 0 b197 3808 00000002
3 4 b0b5 0300 00000586
3 0 b197 0148 00000001
3 5 0000 0200 00000001 NO_CLASS_BOUND
3 0 0000 0410 20030001 INVALID_OPCODE
3 0 b06f 001c 00000002
3 0 b197 3794 00000001
"""

EVENTS = """\
999 pmu start 1
1000 fault unit 0x07 client 0x06 read PTE va 0x0001000000 inst 0x80000000
1001 channel 3 stopped: pushbuffer fetch
"""


class GpuLogTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.classes = gpulog.load_classes()

    def test_names_host_and_engine_methods(self):
        lines = [gpulog.decode_line(line, self.classes)[0] for line in RAW.splitlines()]
        self.assertIn('MAXWELL_CHANNEL_GPFIFO_A SET_OBJECT 0000b197', lines[0])
        self.assertIn('MAXWELL_B SET_REPORT_SEMAPHORE_A 00000001', lines[2])
        self.assertIn('MAXWELL_B SET_STREAM_OUT_BUFFER_ADDRESS_A(0)', lines[3])
        self.assertIn('MAXWELL_B CALL_MME_MACRO(1)', lines[4])
        self.assertIn('MAXWELL_DMA_COPY_A LAUNCH_DMA 00000586', lines[5])
        self.assertIn('MAXWELL_CHANNEL_GPFIFO_A SEMAPHORED 00000002', lines[9])

    def test_flags_methods_the_class_does_not_have(self):
        flags = [gpulog.decode_line(line, self.classes)[1] for line in RAW.splitlines()]
        self.assertEqual(flags[6], 'NOT_IN_CLASS')
        self.assertEqual(flags[7], 'NO_CLASS_BOUND')
        self.assertEqual(flags[8], 'INVALID_OPCODE')
        self.assertEqual(flags[10], 'NOT_IN_CLASS', "the Fermi MME's 128 scratch words end at 0x35fc")
        self.assertEqual(flags.count(None), 7)

    def test_run_summary(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            (run / 'gpu-methods.raw').write_text(RAW)
            (run / 'gpu-events.log').write_text(EVENTS)
            summary = gpulog.summarize(run)
            self.assertTrue((run / 'gpu-methods.log').exists())
        self.assertIn('11 methods', summary)
        self.assertIn('NOT_IN_CLASS 2', summary)
        self.assertIn('2 faults or stopped channels; first: fault unit 0x07', summary)


if __name__ == '__main__':
    unittest.main()
