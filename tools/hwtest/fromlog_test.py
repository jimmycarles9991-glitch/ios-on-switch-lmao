"""tools/hwtest/fromlog.py over lines in boot 14's shape."""

import tempfile
import unittest
from pathlib import Path

from tools.hwtest import fromlog, hwtest
from tools.hwtest.reports import history

LINES = [
    'HWTEST gpu.clear.formats PASS dev=raw raw_checks=228 raw_paths=raw/392 hw=NVIDIA_Tegra_X1 ms=43632.9',
    'HWTEST metal.rgba16f_rounding FAIL halves=3800_3800_37ff_37ff hw=NVIDIA ms=28.5 why=reads_3800_3800_37ff_3800',
    'HWTEST decode.vp9_4k SKIP ms=31.9 why=VTIsHardwareDecodeSupported(VP9)_is_false',
    'noise',
]


class FromLog(unittest.TestCase):
    def test_lines_become_entries(self):
        report = fromlog.report(LINES, {'build': '24B119'}, 'B00T', '2026-11-09T00:13:25.000Z')
        entries = {e['id']: e for e in report['results']}
        self.assertEqual(list(entries), ['gpu.clear.formats', 'metal.rgba16f_rounding', 'decode.vp9_4k'])
        clear = entries['gpu.clear.formats']
        self.assertEqual(clear['metrics'], {'dev': 'raw', 'raw_checks': 228, 'raw_paths': 'raw/392'})
        self.assertEqual(
            (clear['proof'], clear['milliseconds'], clear['subsystem']), ('NVIDIA_Tegra_X1', 43632.9, 'Metal / GPU')
        )
        self.assertEqual(entries['metal.rgba16f_rounding']['detail'], 'reads 3800 3800 37ff 3800')
        self.assertEqual(entries['decode.vp9_4k']['subsystem'], '')
        self.assertEqual(report['summary'], {'pass': 1, 'fail': 1, 'skip': 1, 'crashed': 0, 'planned': 3})
        self.assertEqual(report['run']['status'], 'running', 'no suite line: the run never finished')
        finished = fromlog.report([*LINES, 'HWTEST suite FAIL pass=1 fail=1 skip=1 ios=27 ms=1.0'], {}, 'B', '')
        self.assertEqual(finished['run']['status'], 'done')

    def test_import_lands_in_the_history_as_a_comparable_run(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'run.txt').write_text('\n'.join(LINES) + '\n')
            (root / 'boot.txt').write_text(
                '2026-11-08 16:13:25.098 Ts [0:0] === system boot: 9F6EC579-6EC5-451F-8CB0-BE5AA168E6A1\n'
            )
            argv = ['--history', str(root / 'h'), '--import-log', str(root / 'run.txt'), '--boot-log']
            self.assertEqual(hwtest.main([*argv, str(root / 'boot.txt'), '--build', '24B119']), 0)
            [run] = history(root / 'h')
            self.assertEqual(run.key, ('com.brunostjohn.switch.hwtest', '24B119'))
            self.assertEqual(run.run['boot_uuid'], '9F6EC579-6EC5-451F-8CB0-BE5AA168E6A1')
            self.assertTrue(run.started.endswith('Z'))


if __name__ == '__main__':
    unittest.main()
