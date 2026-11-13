"""Kernel GR lines decoded, given to the experiment that died of them, and a dead bench said in one line."""

import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from tools.benchloop import doctor, gr_errors, loop, transport

# Lines the device logged on 11 Nov 19:34:57 while the calibration batch's `known` experiment ran on pack v15
REGISTER_COUNT = (
    'Nov 11 19:34:57.198470 kernel.development[0] <Notice>: tegra-gm20b 57000000.gpu: gr: DATA_ERROR 000000b5 [] ch 35 '
    '[00e9014000 SwitchHWTest] subc 0 class b197 mthd 0d78 data 00000004'
)
CHANNEL_KILLED = (
    'Nov 11 19:34:57.199991 kernel.development[0] <Notice>: [ 1832.316407]: tegra-gm20b: GPU error in SwitchHWTest '
    '(channel 35): channel killed, GPU running: gr: DATA_ERROR 000000b5 [] ch 35 [00e9014000 SwitchHWTest] subc 0 '
    'class b197 mthd 0d78 data 00000004'
)
KNOWN_LINE = (
    'Nov 11 19:34:57.260882 SwitchHWTest[466] <Notice>: BENCH cal known killed ms=111.1 why=the_channel_was_dead_after_'
    "the_experiment's_last_fence_(a_trap_the_fence_missed)"
)
TRAP = (
    'tegra-gm20b 57000000.gpu: gr: GPC0/TPC0/MP trap: global 00000004 [MULTIPLE_WARP_ERRORS] warp 0009 '
    '[ILLEGAL_INSTR_ENCODING]'
)
DECODED = 'DATA_ERROR 0xb5 ILLEGAL_SHADER_REGISTER_COUNT1 at class b197 method 0x0d78 data 0x00000004'


class Stream:
    def __init__(self, lines: list[str]) -> None:
        self._lines = lines

    def lines(self, until: float) -> list[str]:
        del until
        return self._lines


class GrErrorsTest(unittest.TestCase):
    def test_a_class_error_nouveau_leaves_unnamed_is_named(self) -> None:
        self.assertEqual(gr_errors.decode(REGISTER_COUNT), DECODED)
        self.assertEqual(gr_errors.names()[0x59], 'ITERATED_BLEND_TARGET_COUNT')

    def test_a_trap_line_keeps_its_text(self) -> None:
        self.assertIn('ILLEGAL_INSTR_ENCODING', gr_errors.decode(TRAP))
        self.assertIsNone(gr_errors.decode('tegra-gm20b: fifo: errored - disabling channel'))

    def test_the_lines_go_to_the_experiment_logged_after_them(self) -> None:
        b = {'batch': 'cal', 'experiments': [{'id': 'noop', 'kind': 'noop'}, {'id': 'known', 'kind': 'known'}]}
        stream = Stream(
            ['BENCH cal noop ok runs=0:2343', REGISTER_COUNT, CHANNEL_KILLED, KNOWN_LINE, 'BENCH cal done ok=1']
        )
        outcome = loop.wait(SimpleNamespace(root='/bench'), b, stream)
        self.assertEqual(outcome.kernel, {'known': [DECODED]})
        self.assertEqual(outcome.log['known']['status'], 'killed')

    def test_a_killed_known_program_is_one_line(self) -> None:
        ok, lines = doctor.judge({'known': {'status': 'killed', 'runs': [], 'kernel': [DECODED]}})
        self.assertFalse(ok)
        self.assertTrue(lines[0].startswith('BENCH DEAD:'), lines)
        self.assertIn('ILLEGAL_SHADER_REGISTER_COUNT1', lines[0])
        self.assertIsNone(doctor.dead_bench({'status': 'ok'}))

    def test_the_apps_protocol_is_read_before_any_batch(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            (Path(scratch) / 'status.json').write_text('{"protocol": "benchloop/1"}')
            self.assertEqual(doctor.app_protocol(transport.Local(Path(scratch))), 'benchloop/1')
            self.assertIsNone(doctor.app_protocol(transport.Local(Path(scratch) / 'none')))

    def test_the_output_kernel_informs_without_deciding(self) -> None:
        texels = {'distinct': [{'value': doctor.KNOWN_TEXEL}]}
        ok_result = {'status': 'ok', 'runs': [{'gpu_ns': [5000]}], 'texels': texels}
        results = {e['id']: ok_result for e in doctor.calibration()}
        results['output-kernel'] = {'status': 'killed', 'runs': []}
        ok, lines = doctor.judge(results)
        self.assertTrue(ok, lines)
        self.assertTrue(any(line.startswith('output-kernel: FAIL (informative') for line in lines), lines)


if __name__ == '__main__':
    unittest.main()
