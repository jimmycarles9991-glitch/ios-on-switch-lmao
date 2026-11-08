"""A memo filled by one run gives the next run the same tree without translating, and any changed key input misses."""

import hashlib
import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

DRIVER = Path(__file__).with_name('translate_tree.py')
SPEC = importlib.util.spec_from_file_location('translate_tree', DRIVER)
translate_tree = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(translate_tree)

MACHO = b'\xcf\xfa\xed\xfe'
FAKE_TOOL = """#!{python}
import hashlib, json, sys
from pathlib import Path
with open(sys.argv[0] + '.calls', 'a') as calls:
    calls.write(' '.join(sys.argv[1:2]) + '\\n')
if sys.argv[1] == 'pretranslate':
    report, src, out = sys.argv[3], Path(sys.argv[4]), Path(sys.argv[5])
    data = src.read_bytes()
    out.write_bytes(b'translated {salt} ' + data)
    slice = dict(status='translated', cdhash_in=hashlib.sha1(data).hexdigest(),
                 cdhash_out=hashlib.sha1(out.read_bytes()).hexdigest())
    Path(report).write_text(json.dumps(dict(slices=[slice])))
"""


def tree(root: Path) -> dict[str, str]:
    return {
        str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in sorted(root.rglob('*'))
        if p.is_file() and '.work' not in p.parts
    }


class MemoRunTest(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.memo = translate_tree.Memo(self.dir / 'memo')
        self.produced = 0

    def produce(self, dst: Path, data: bytes) -> dict:
        self.produced += 1
        dst.write_bytes(data)
        return {'value': data.decode()}

    def test_a_kept_key_restores_the_output_without_producing(self):
        dst = self.dir / 'out'
        self.memo.run(['a', 'b'], dst, lambda: self.produce(dst, b'one'))
        dst.unlink()
        self.assertEqual(self.memo.run(['a', 'b'], dst, lambda: self.produce(dst, b'two')), {'value': 'one'})
        self.assertEqual((self.produced, dst.read_bytes()), (1, b'one'))

    def test_each_changed_part_misses(self):
        dst = self.dir / 'out'
        self.memo.run(['dsc', 'cache', 'plan', 'pairs', 'tool'], dst, lambda: self.produce(dst, b'x'))
        for i in range(5):
            parts = ['dsc', 'cache', 'plan', 'pairs', 'tool']
            parts[i] += '!'
            self.memo.run(parts, dst, lambda: self.produce(dst, b'x'))
        self.assertEqual(self.produced, 6)

    def test_an_unhashable_part_misses_and_keeps_nothing(self):
        dst = self.dir / 'out'
        self.memo.run(['a', None], dst, lambda: self.produce(dst, b'x'))
        self.memo.run(['a', None], dst, lambda: self.produce(dst, b'x'))
        self.assertEqual(self.produced, 2)
        self.assertFalse((self.dir / 'memo').exists())

    def test_a_corrupt_output_misses(self):
        dst = self.dir / 'out'
        self.memo.run(['a'], dst, lambda: self.produce(dst, b'good'))
        (out,) = (self.dir / 'memo').glob('*.out')
        out.write_bytes(b'bad')
        self.memo.run(['a'], dst, lambda: self.produce(dst, b'good'))
        self.assertEqual((self.produced, dst.read_bytes()), (2, b'good'))


class WarmThenColdTest(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.volume = self.dir / 'volume'
        (self.volume / 'usr/bin').mkdir(parents=True)
        (self.volume / 'usr/bin/a').write_bytes(MACHO + b'a')
        (self.volume / 'usr/bin/b').write_bytes(MACHO + b'b')
        self.memo = self.dir / 'memo'
        self.tool = self.tool_with_salt('one')

    def tool_with_salt(self, salt: str) -> Path:
        tool = self.dir / f'arm64e-{salt}'
        tool.write_text(FAKE_TOOL.format(python=sys.executable, salt=salt))
        tool.chmod(0o755)
        return tool

    def run_driver(self, out: str, tool: Path) -> Path:
        out_dir = self.dir / out
        command = [sys.executable, str(DRIVER), '--role', 'os', '--tool', str(tool), '--volume', str(self.volume)]
        subprocess.run([*command, '--out', str(out_dir), '--memo', str(self.memo)], check=True, capture_output=True)
        return out_dir

    def calls(self, tool: Path) -> list[str]:
        path = Path(str(tool) + '.calls')
        return path.read_text().split() if path.exists() else []

    def test_a_warm_memo_gives_the_cold_tree_without_translating(self):
        warm = self.run_driver('warm', self.tool)
        self.assertEqual(len(self.calls(self.tool)), 2)
        cold = self.run_driver('cold', self.tool)
        self.assertEqual(len(self.calls(self.tool)), 2)
        self.assertEqual(tree(warm), tree(cold))

    def test_progress_counts_the_files_only_when_asked(self):
        command = [sys.executable, str(DRIVER), '--role', 'os', '--tool', str(self.tool), '--volume', str(self.volume)]
        quiet = subprocess.run([*command, '--out', str(self.dir / 'quiet')], check=True, capture_output=True, text=True)
        told = subprocess.run(
            [*command, '--out', str(self.dir / 'told'), '--progress'], check=True, capture_output=True, text=True
        )
        self.assertNotIn('progress', quiet.stdout)
        self.assertEqual(
            [line for line in told.stdout.splitlines() if line.startswith('progress')],
            ['progress finding the Mach-O files', 'progress 0/2 files', 'progress 1/2 files', 'progress 2/2 files'],
        )

    def test_a_changed_input_is_translated_again(self):
        self.run_driver('warm', self.tool)
        (self.volume / 'usr/bin/a').write_bytes(MACHO + b'changed')
        self.run_driver('cold', self.tool)
        self.assertEqual(len(self.calls(self.tool)), 3)

    def test_another_translator_is_translated_again(self):
        self.run_driver('warm', self.tool)
        other = self.tool_with_salt('two')
        self.run_driver('cold', other)
        self.assertEqual(len(self.calls(other)), 2)


if __name__ == '__main__':
    unittest.main()
