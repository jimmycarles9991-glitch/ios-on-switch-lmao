"""afcclient's command lines as the loop drives it, and the log lines it falls back on."""

import subprocess
import tempfile
import unittest
from pathlib import Path

from tools.benchloop import golden, loop, transport

AFCCLIENT_OPTIONS = {'-u': 1, '--udid': 1, '--documents': 1, '--container': 1, '-n': 0, '-d': 0}


def invalid_option(command: list[str]) -> str | None:
    """Give the argument afcclient 1.4's getopt_long rejects: it permutes argv, so any dash before `--` is its own."""
    args = iter(command[1:])
    for arg in args:
        if arg == '--':
            return None
        if arg.startswith('-'):
            if arg not in AFCCLIENT_OPTIONS:
                return arg
            for _ in range(AFCCLIENT_OPTIONS[arg]):
                next(args, None)
    return None


class Recorder:
    def __init__(self, outputs: dict[str, str] | None = None) -> None:
        self.calls: list[list[str]] = []
        self.outputs = outputs or {}

    def __call__(self, command: list[str], **_: object) -> subprocess.CompletedProcess:
        self.calls.append(command)
        if bad := invalid_option(command):
            return subprocess.CompletedProcess(command, 2, '', f'afcclient: invalid option -- {bad.lstrip("-")}\n')
        verb = command[command.index('ls')] if 'ls' in command else ''
        out = self.outputs.get(command[-1] if verb else '', '')
        failed = verb and command[-1] not in self.outputs
        return subprocess.CompletedProcess(command, 1 if failed else 0, out, 'Error: no such file' if failed else '')


class AfcTest(unittest.TestCase):
    def test_documents_root_goes_through_house_arrest(self) -> None:
        run = Recorder({'/Documents': '.\n..\nbenchloop\n', '/Documents/benchloop': 'inbox\n'})
        afc = transport.Afc(udid='u1', documents=transport.BUNDLES[0], run=run)
        self.assertEqual(afc.root, '/Documents/benchloop')
        afc.mkdir('/Documents/benchloop/inbox')
        self.assertEqual(
            run.calls[-1],
            ['afcclient', '-u', 'u1', '--documents', transport.BUNDLES[0], '--', 'mkdir', '/Documents/benchloop/inbox'],
        )
        self.assertEqual(afc.ls('/Documents'), ['benchloop'])

    def test_media_root_is_plain_afc(self) -> None:
        run = Recorder()
        afc = transport.Afc(run=run)
        afc.mv('/Switch/benchloop/inbox/.b.json.part', '/Switch/benchloop/inbox/b.json')
        self.assertEqual(run.calls[-1][:3], ['afcclient', '--', 'mv'])
        self.assertEqual(afc.root, '/Switch/benchloop')

    def test_verb_flags_reach_the_verb(self) -> None:
        run = Recorder()
        afc = transport.Afc(udid='u1', documents=transport.BUNDLES[0], run=run)
        with tempfile.TemporaryDirectory() as scratch:
            afc.get('/Documents/benchloop/outbox/b', Path(scratch) / 'b')
        afc.rm('/Documents/benchloop/outbox/b')
        self.assertEqual(run.calls[0][-4:-2], ['get', '-r'])
        self.assertEqual(run.calls[1][-3:], ['rm', '-rf', '/Documents/benchloop/outbox/b'])

    def test_a_missing_device_is_an_error(self) -> None:
        def absent(command: list[str], **_: object) -> subprocess.CompletedProcess:
            return subprocess.CompletedProcess(command, 0, '', 'No device found. Plug in a device')

        with self.assertRaises(transport.TransportError):
            transport.Afc(run=absent).put(Path('x'), '/Switch/benchloop/inbox/x')

    def test_batch_is_renamed_into_the_inbox_after_it_is_whole(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            seen: list[list[str]] = []
            local = transport.Local(
                Path(scratch), lambda: seen.append(sorted(p.name for p in Path(scratch, 'inbox').iterdir()))
            )
            loop.push(local, golden.golden_batch())
            self.assertEqual(seen, [['golden.json']])


class LogLineTest(unittest.TestCase):
    def test_bench_line_parses(self) -> None:
        line = 'SwitchHWTest[201] <Notice>: BENCH lat-0 e12 ok runs=1:5120,2:9800 crc=1a2b3c4d ms=12.5'
        batch, experiment, result = loop.parse_log_line(line)
        self.assertEqual((batch, experiment, result['status']), ('lat-0', 'e12', 'ok'))
        self.assertEqual([r['gpu_ns'] for r in result['runs']], [[5120], [9800]])
        self.assertEqual(result['texels'], {'crc32': '1a2b3c4d'})
        self.assertIsNone(loop.parse_log_line('BENCH selfcheck PASS noop=1ns'))


if __name__ == '__main__':
    unittest.main()
