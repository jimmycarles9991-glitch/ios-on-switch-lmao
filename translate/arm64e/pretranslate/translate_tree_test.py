"""A memo filled by one run gives the next run the same tree without translating, and any changed key input misses."""

import hashlib
import importlib.util
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

DRIVER = Path(__file__).with_name('translate_tree.py')
EDITS_24B119 = Path(__file__).with_name('edits_24B119.json')
SPEC = importlib.util.spec_from_file_location('translate_tree', DRIVER)
translate_tree = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(translate_tree)

MACHO = b'\xcf\xfa\xed\xfe'
FAKE_TOOL = """#!{python}
import hashlib, json, sys
from pathlib import Path
with open(sys.argv[0] + '.calls', 'a') as calls:
    calls.write(' '.join(sys.argv[1:]) + '\\n')
if sys.argv[1] == 'pretranslate':
    args = sys.argv[2:]
    report, edits = None, []
    i = 0
    while i < len(args) and args[i].startswith('-'):
        if args[i] == '-r':
            report = args[i + 1]
            i += 2
        elif args[i] == '-e':
            edits.append(args[i + 1])
            i += 2
        else:
            sys.exit(2)
    src, out = Path(args[i]), Path(args[i + 1])
    raw = src.read_bytes()
    buf = bytearray(raw)
    for spec in edits:
        off_s, stock_h, at_s, patched_h = spec.split(':')
        off, at = int(off_s, 0), int(at_s, 0)
        stock, patched = bytes.fromhex(stock_h), bytes.fromhex(patched_h)
        if bytes(buf[off : off + len(stock)]) != stock:
            sys.exit(1)
        if patched:
            buf[off + at : off + at + len(patched)] = patched
    out.write_bytes(b'translated {salt} ' + bytes(buf))
    slice = dict(status='translated', cdhash_in=hashlib.sha1(raw).hexdigest(),
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
        return path.read_text().splitlines() if path.exists() else []

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

    def test_edits_patch_synthetic_file_preserve_original_cdhash_and_bust_memo(self):
        stock = b'\x13\x00\x80\xd2\x55\x7e\x91\xd2'
        patched = b'\x15\x20\x80\xd2\x15\x04\xa2\xf2'
        (self.volume / 'usr/bin/a').write_bytes(MACHO + stock)
        edits_file = self.dir / 'edits.json'
        edits_file.write_text(
            json.dumps(
                {'files': {'/usr/bin/a': [{'fileoff': '0x4', 'stock': stock.hex(), 'at': 0, 'patched': patched.hex()}]}}
            )
        )
        raw_cdhash = hashlib.sha1(MACHO + stock).hexdigest()

        base_out = self.run_driver('plain', self.tool)
        self.assertEqual(len(self.calls(self.tool)), 2)

        edited_out = self.dir / 'edited'
        cmd = [
            sys.executable,
            str(DRIVER),
            '--role',
            'os',
            '--tool',
            str(self.tool),
            '--volume',
            str(self.volume),
            '--out',
            str(edited_out),
            '--memo',
            str(self.memo),
            '--edits',
            str(edits_file),
        ]
        subprocess.run(cmd, check=True, capture_output=True)
        self.assertEqual(len(self.calls(self.tool)), 3)
        self.assertIn(patched, (edited_out / 'files/usr/bin/a').read_bytes())
        self.assertNotEqual((base_out / 'files/usr/bin/a').read_bytes(), (edited_out / 'files/usr/bin/a').read_bytes())
        pairs = dict(line.split() for line in (edited_out / 'os.cdhash-pairs').read_text().splitlines())
        self.assertIn(raw_cdhash, pairs)

        rerun_out = self.dir / 'edited-rerun'
        subprocess.run(
            [*cmd[:-4], '--out', str(rerun_out), '--memo', str(self.memo), '--edits', str(edits_file)],
            check=True,
            capture_output=True,
        )
        self.assertEqual(len(self.calls(self.tool)), 3)

        bad_edits = self.dir / 'missing.json'
        bad_edits.write_text(
            json.dumps(
                {'files': {'/usr/bin/missing': [{'fileoff': '0x4', 'stock': stock.hex(), 'at': 0, 'patched': ''}]}}
            )
        )
        bad = subprocess.run([*cmd[:-2], '--edits', str(bad_edits)], capture_output=True, text=True)
        self.assertNotEqual(bad.returncode, 0)
        self.assertIn('/usr/bin/missing', bad.stderr)

    def test_load_edits_rejects_whitespace_hex_negative_offset_and_canonicalizes_fileoff(self):
        canon = self.dir / 'canon.json'
        canon.write_text(
            json.dumps({'files': {'/usr/bin/a': [{'fileoff': 4, 'stock': '130080D2', 'at': 0, 'patched': ''}]}})
        )
        hex_form = self.dir / 'hex.json'
        hex_form.write_text(
            json.dumps({'files': {'/usr/bin/a': [{'fileoff': '0x04', 'stock': '130080d2', 'at': 0, 'patched': ''}]}})
        )
        e1 = translate_tree.load_edits(canon)
        e2 = translate_tree.load_edits(hex_form)
        self.assertEqual(e1, e2)
        self.assertEqual(e1['/usr/bin/a'][0]['fileoff'], '0x4')

        for bad_entry in (
            {'fileoff': '0x4', 'stock': '13 00 80 d2', 'at': 0, 'patched': ''},
            {'fileoff': '0x4', 'stock': '130080d2', 'at': 0, 'patched': '1f 20 03 d5'},
            {'fileoff': '-4', 'stock': '130080d2', 'at': 0, 'patched': ''},
        ):
            p = self.dir / 'bad.json'
            p.write_text(json.dumps({'files': {'/usr/bin/a': [bad_entry]}}))
            with self.assertRaises(ValueError):
                translate_tree.load_edits(p)


class Locationd24B119EditTest(unittest.TestCase):
    @staticmethod
    def _run_case(entries: list[dict], patched: bool) -> tuple[int, int, int]:
        text: dict[int, int] = {}
        for entry in entries:
            base = int(entry['fileoff'], 0)
            buf = bytearray(bytes.fromhex(entry['stock']))
            if patched and entry['patched']:
                at = int(entry['at'])
                rep = bytes.fromhex(entry['patched'])
                buf[at : at + len(rep)] = rep
            for off in range(0, len(buf), 4):
                text[base + off] = struct.unpack_from('<I', buf, off)[0]
        pc, tail = 0x7F0A3C, 0x7F0F1C
        x19, x21 = 0, 0
        for _ in range(16):
            if pc == tail:
                break
            insn = text[pc]
            rd, hw, imm16 = insn & 0x1F, (insn >> 21) & 3, (insn >> 5) & 0xFFFF
            if (insn & 0x7F800000) == 0x52800000 and rd in (19, 21):
                if rd == 19:
                    x19 = imm16 << (16 * hw)
                else:
                    x21 = imm16 << (16 * hw)
            elif (insn & 0x7F800000) == 0x72800000 and rd in (19, 21):
                if rd == 19:
                    x19 = (x19 & ~(0xFFFF << (16 * hw))) | (imm16 << (16 * hw))
                else:
                    x21 = (x21 & ~(0xFFFF << (16 * hw))) | (imm16 << (16 * hw))
            elif (insn & 0xFC000000) == 0x14000000:
                imm26 = insn & 0x3FFFFFF
                if imm26 & 0x2000000:
                    imm26 -= 0x4000000
                pc += imm26 * 4
                continue
            else:
                raise AssertionError(f'unexpected instruction {insn:#x} at {pc:#x}')
            pc += 4
        return pc, x19, x21

    def test_24b119_locationd_edit_clears_exact_coprocessor_bits(self):
        edits = translate_tree.load_edits(EDITS_24B119)
        self.assertIn('/usr/libexec/locationd', edits)
        entries = edits['/usr/libexec/locationd']
        end_stock, x19_stock, x21_stock = self._run_case(entries, patched=False)
        self.assertEqual((end_stock, x19_stock, x21_stock), (0x7F0F1C, 0, 0x0000001314618BF2))

        end_patched, x19_patched, x21_patched = self._run_case(entries, patched=True)
        self.assertEqual((end_patched, x19_patched), (0x7F0F1C, 0))
        self.assertEqual(x21_patched & ~0x0000001314618BF2, 0)
        cleared = 0
        for bit in (1, 4, 5, 6, 7, 9, 11, 15, 16, 22, 26, 33, 36):
            cleared |= 1 << bit
        self.assertEqual(0x0000001314618BF2 & ~x21_patched, cleared)
        self.assertEqual(x21_patched, (1 << 8) | (1 << 21) | (1 << 28) | (1 << 32))


if __name__ == '__main__':
    unittest.main()
