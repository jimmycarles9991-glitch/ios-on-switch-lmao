"""boot.py's nested Bazel calls, against a fake bazel that parses its command line the way Bazel does."""

import os
import re
import subprocess
import unittest
from pathlib import Path
from unittest import mock

from tools.emu import boot

OUTPUT_BASE = '/fake/output_base'
EXECUTION_ROOT = '/fake/output_base/execroot/_main'
FILES = {'//:boot': 'bazel-out/cfg/bin/boot/ios', '//kernel:prelinked': 'bazel-out/cfg/bin/kernel/kernel.macho'}
LABEL = re.compile(r'^//[\w/.-]*:[\w/.-]+$')


class FakeBazel:
    """Records each call; a query command's words that are not flags form one expression, as Bazel joins them."""

    def __init__(self) -> None:
        self.calls = []

    def __call__(self, command: list, **_: object) -> subprocess.CompletedProcess:
        binary, startup, verb, *rest = command
        self.calls.append((verb, rest))
        if binary != 'bazel' or startup != f'--output_base={OUTPUT_BASE}':
            return subprocess.CompletedProcess(command, 2, '')
        words = [word for word in rest if not word.startswith('--')]
        if verb == 'cquery':
            expression = ' '.join(words)
            if not LABEL.match(expression):
                return subprocess.CompletedProcess(command, 2, f'syntax error in {expression!r}')
            return subprocess.CompletedProcess(command, 0, FILES[expression] + '\n')
        if verb == 'info':
            return subprocess.CompletedProcess(command, 0, EXECUTION_ROOT + '\n')
        return subprocess.CompletedProcess(command, 0, '')


class BuildPackageTest(unittest.TestCase):
    def setUp(self):
        runfiles = f'{EXECUTION_ROOT}/bazel-out/cfg/bin/tools/emu/boot.runfiles'
        environment = mock.patch.dict(os.environ, {'RUNFILES_DIR': runfiles, 'BUILD_WORKSPACE_DIRECTORY': '/fake/ws'})
        environment.start()
        self.addCleanup(environment.stop)
        self.bazel = FakeBazel()
        run = mock.patch.object(boot.subprocess, 'run', self.bazel)
        run.start()
        self.addCleanup(run.stop)

    def test_builds_the_kind_and_finds_both_outputs(self):
        package, kernel = boot.build_package('ios27', ['--config=agent'])
        self.assertEqual(package, Path(EXECUTION_ROOT) / FILES['//:boot'])
        self.assertEqual(kernel, Path(EXECUTION_ROOT) / FILES['//kernel:prelinked'])
        for verb, rest in self.bazel.calls:
            self.assertIn('--//ipsw:kind=24B119', rest, verb)
            self.assertIn('--config=agent', rest, verb)
        self.assertEqual([verb for verb, _ in self.bazel.calls], ['build', 'info', 'cquery', 'cquery'])
        self.assertEqual(self.bazel.calls[0][1][-2:], ['//:boot', '//kernel:prelinked'])

    def test_every_kind_has_a_build(self):
        for kind in boot.KINDS:
            boot.build_package(kind, [])
        self.assertEqual(len(self.bazel.calls), 4 * len(boot.KINDS))


if __name__ == '__main__':
    unittest.main()
