"""Pinned Swift tools: format convergence, non-mutating checks and strict lint diagnostics."""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from python.runfiles import runfiles


class SwiftTools(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.runfiles = runfiles.Create()
        cls.formatter = cls.locate(os.environ['SWIFTFORMAT'])
        cls.format_config = cls.locate(os.environ['SWIFTFORMAT_CONFIG'])
        cls.linter = cls.locate(os.environ['SWIFTLINT'])
        cls.lint_config = cls.locate(os.environ['SWIFTLINT_CONFIG'])

    @classmethod
    def locate(cls, name: str) -> Path:
        path = cls.runfiles.Rlocation(name)
        if not path:
            raise FileNotFoundError(name)
        return Path(path)

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(dir=os.environ['TEST_TMPDIR'])
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def format(self, path: Path, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [self.formatter, '--config', self.format_config, '--cache', 'ignore', '--symlinks', 'follow', *args, path],
            capture_output=True,
            text=True,
            check=False,
        )

    def lint(self, path: Path) -> subprocess.CompletedProcess:
        return subprocess.run(
            [self.linter, 'lint', '--strict', '--quiet', '--no-cache', '--config', self.lint_config, path],
            capture_output=True,
            text=True,
            check=False,
        )

    def test_tool_versions_match_pins(self) -> None:
        for tool, version in ((self.formatter, '0.64.7'), (self.linter, '0.65.1')):
            with self.subTest(tool=tool):
                result = subprocess.run([tool, '--version'], capture_output=True, text=True, check=False)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(result.stdout.strip(), version)

    def test_formatting_converges_for_every_repository_swift_file(self) -> None:
        sources = os.environ['SWIFT_SOURCES'].split()
        self.assertGreater(len(sources), 0)
        for source in sources:
            target = self.root / source
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(self.locate(source), target)
        first = self.format(self.root)
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        self.assertNotIn('warning:', first.stdout + first.stderr)
        before = {p: p.read_bytes() for p in self.root.rglob('*.swift')}
        second = self.format(self.root)
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertEqual([str(p.relative_to(self.root)) for p, data in before.items() if p.read_bytes() != data], [])

    def test_format_check_does_not_modify_source_and_follows_symlinks(self) -> None:
        source = self.root / 'Example.swift'
        original = 'struct Example{\nlet value=1\n}\n'
        source.write_text(original)
        linked = self.root / 'Linked.swift'
        linked.symlink_to(source)
        failed = self.format(linked, '--lint')
        self.assertNotEqual(failed.returncode, 0, failed.stdout + failed.stderr)
        self.assertEqual(source.read_text(), original)
        formatted = self.format(source)
        self.assertEqual(formatted.returncode, 0, formatted.stdout + formatted.stderr)
        self.assertEqual(source.read_text(), 'struct Example {\n    let value = 1\n}\n')
        passed = self.format(linked, '--lint')
        self.assertEqual(passed.returncode, 0, passed.stdout + passed.stderr)

    def test_linter_rejects_unsafe_code_without_modifying_it(self) -> None:
        source = self.root / 'Example.swift'
        original = 'func value(_ input: Any) -> String {\n    input as! String\n}\n'
        source.write_text(original)
        result = self.lint(source)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('force_cast', result.stdout + result.stderr)
        self.assertEqual(source.read_text(), original)

    def test_linter_accepts_formatted_safe_code(self) -> None:
        source = self.root / 'Example.swift'
        source.write_text('func value(_ input: Int) -> Int {\n    input + 1\n}\n')
        result = self.lint(source)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn('warning:', result.stdout + result.stderr)

    def test_formatter_handles_shifts_inside_ternaries_and_nested_generics(self) -> None:
        source = self.root / 'Example.swift'
        source.write_text(
            'func value(_ wide: Bool) -> UInt16 {\n'
            '    let codes: Array<Array<UInt16>> = [[1, 2]]\n'
            '    return wide\n'
            '        ? codes[0][0] | codes[0][1] << 8\n'
            '        : codes[0][0]\n'
            '}\n'
        )
        first = self.format(source)
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        formatted = source.read_text()
        self.assertIn('codes[0][0] | codes[0][1] << 8', formatted)
        second = self.format(source)
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertEqual(source.read_text(), formatted)

    def test_formatter_rejects_incomplete_source(self) -> None:
        source = self.root / 'Example.swift'
        source.write_text('struct Example {\n')
        result = self.format(source)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(source.read_text(), 'struct Example {\n')

    def test_linter_rejects_opt_in_warning(self) -> None:
        source = self.root / 'Example.swift'
        source.write_text(
            'func ignore(_ action: () throws -> Void) {\n    do {\n        try action()\n    } catch {}\n}\n'
        )
        result = self.lint(source)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('no_empty_block', result.stdout + result.stderr)

    def test_linter_checks_symlinked_inputs(self) -> None:
        source = self.root / 'Example.swift'
        source.write_text('func value(_ input: Any) -> String {\n    input as! String\n}\n')
        linked = self.root / 'Linked.swift'
        linked.symlink_to(source)
        result = self.lint(linked)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('force_cast', result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
