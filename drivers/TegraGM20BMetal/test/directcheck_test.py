"""directcheck (harness/directcheck.mm) over a sample of the pack's states, as the runtime makes them.

Each sampled state's record from the runtime's direct compile must be the pack's byte for byte; a state the pack set
aside must be refused. The sample is fixed by the pack's bucket corpora: of their lines, the PLAIN and the EXTENDED
(those naming an extended-range colour, 550 to 555, which render through their RGBA16Float stand-ins) of lowest SHA-256,
so a line added to the corpus moves at most one line out of it. The first of SHARDS processes makes the driver's own states
as well, and scans the whole pack as the runtime trusts it (pack_scan.h): any finding fails.

usage: directcheck_test.py <directcheck inputs.json>
"""

import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

PLAIN, EXTENDED, SHARDS = 2048, 256, 4
EXTENDED_COLOUR = re.compile(r' c[0-7]=55[0-5]:')
SUMMARY = re.compile(r'^directcheck: (\d+) states made directly: .* (\d+) differ;.*$', re.MULTILINE)
INPUTS: dict = {}


def sample(corpora: list[Path]) -> list[str]:
    lines = {line for c in corpora for line in c.read_text().splitlines() if line.startswith('R ') and ' ok=1 ' in line}
    ranked = sorted(lines, key=lambda line: hashlib.sha256(line.encode()).digest())
    extended = [line for line in ranked if EXTENDED_COLOUR.search(line.split(' | ')[0])]
    plain = [line for line in ranked if not EXTENDED_COLOUR.search(line.split(' | ')[0])]
    return extended[:EXTENDED] + plain[:PLAIN]


class Directcheck(unittest.TestCase):
    def test_records_are_the_packs(self) -> None:
        work = Path(tempfile.mkdtemp(dir=os.environ.get('TEST_TMPDIR')))
        kept = Path(os.environ.get('TEST_UNDECLARED_OUTPUTS_DIR', work))
        tools = Path(INPUTS['tools'])
        for name in ('directcheck', 'libmock_iokit.dylib', 'own.metallib'):
            shutil.copyfile(tools / name, work / name)
        (work / 'directcheck').chmod(0o755)
        lines = sample([Path(c) for c in INPUTS['corpora']])
        self.assertTrue(
            any(EXTENDED_COLOUR.search(line) for line in lines), 'the corpora name no extended-range colour'
        )
        env = dict(os.environ, PROBE_LIB_ROOT=str(Path(INPUTS['lib_root']).resolve()))
        runs = []
        for shard in range(SHARDS):
            corpus = work / f'corpus{shard}.log'
            corpus.write_text('\n'.join(lines[shard::SHARDS]) + '\n')
            own = {'PROBE_OWN_LIB': str(work / 'own.metallib'), 'DIRECTCHECK_SCAN': '1'} if shard == 0 else {}
            command = [work / 'directcheck', INPUTS['qc'], corpus]
            runs.append(
                subprocess.Popen(
                    [str(c) for c in [*command, INPUTS['pack']]],
                    env=dict(env, **own),
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                )
            )
        states = 0
        report = []
        for shard, run in enumerate(runs):
            stdout, stderr = run.communicate()
            (kept / f'shard{shard}.log').write_text(stdout + stderr)
            summary = SUMMARY.search(stdout)
            self.assertIsNotNone(summary, f'shard {shard} exited {run.returncode}: {stdout[-2000:]}{stderr[-2000:]}')
            states += int(summary.group(1))
            if run.returncode:
                print('\n'.join(line for line in stdout.splitlines() if line.startswith(('DIFFERS', 'SCAN '))))
                report += [line for line in stdout.splitlines() if line.startswith('SCAN ')][:40]
                report.append(f'shard {shard}: {summary.group(0)}')
                report += [line for line in stdout.splitlines() if line.startswith('  ') and ' x ' in line]
        print(f'directcheck: {len(lines)} lines, {states} records made')
        self.assertFalse(report, '\n'.join(report))


if __name__ == '__main__':
    INPUTS.update(json.loads(Path(sys.argv[1]).read_text()))
    unittest.main(argv=sys.argv[:1])
