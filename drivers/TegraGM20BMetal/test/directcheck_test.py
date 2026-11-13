"""directcheck (raw/test/directcheck.mm) over a sample of the raw pack's states, as the runtime makes them.

Each sampled state's record from the runtime's direct compile must be the pack's byte for byte, and its two-plane form's
too for a state of RGB10A8_2P_XR10 colours; a state the pack set aside must be refused. The sample is fixed by the
pack's bucket corpora: of their lines, the PLAIN and the TWO_PLANE (those naming a 550 colour) of lowest SHA-256, so a
line added to the corpus moves at most one line out of it. The first of SHARDS processes makes the raw path's own states
as well.

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

PLAIN, TWO_PLANE, SHARDS = 2048, 256, 4
TWO_PLANE_COLOUR = re.compile(r' c[0-7]=550:')
SUMMARY = re.compile(
    r'^directcheck: (\d+) states made directly: .* (\d+) differ;.* (\d+) of them two-plane forms$', re.MULTILINE
)
# The probe's environment (raw/pack/rawpack.py probe_env) as far as the direct compile reads it
PROBE_ENV = {'MESA_SHADER_CACHE_DISABLE': 'true', 'NMB_ASYNC_PSO': '0', 'NMB_ASYNC_PSO_API': '0'}
INPUTS: dict = {}


def sample(corpora: list[Path]) -> list[str]:
    lines = {line for c in corpora for line in c.read_text().splitlines() if line.startswith('R ') and ' ok=1 ' in line}
    ranked = sorted(lines, key=lambda line: hashlib.sha256(line.encode()).digest())
    two_plane = [line for line in ranked if TWO_PLANE_COLOUR.search(line.split(' | ')[0])]
    plain = [line for line in ranked if not TWO_PLANE_COLOUR.search(line.split(' | ')[0])]
    return two_plane[:TWO_PLANE] + plain[:PLAIN]


class Directcheck(unittest.TestCase):
    def test_records_are_the_packs(self) -> None:
        work = Path(tempfile.mkdtemp(dir=os.environ.get('TEST_TMPDIR')))
        tools = Path(INPUTS['tools'])
        for name in ('directcheck', 'libmock_iokit.dylib', 'libvulkan_nouveau.dylib', 'own.metallib'):
            shutil.copyfile(tools / name, work / name)
        (work / 'directcheck').chmod(0o755)
        lines = sample([Path(c) for c in INPUTS['corpora']])
        self.assertTrue(any(TWO_PLANE_COLOUR.search(line) for line in lines), 'the corpora name no 550 colour')
        env = dict(os.environ, **PROBE_ENV, PROBE_LIB_ROOT=str(Path(INPUTS['lib_root']).resolve()))
        runs = []
        for shard in range(SHARDS):
            corpus, cache = work / f'corpus{shard}.log', work / f'cache{shard}'
            corpus.write_text('\n'.join(lines[shard::SHARDS]) + '\n')
            cache.mkdir()
            own = {'PROBE_OWN_LIB': str(work / 'own.metallib')} if shard == 0 else {}
            command = [work / 'directcheck', work / 'libvulkan_nouveau.dylib', cache, INPUTS['qc'], corpus]
            runs.append(
                subprocess.Popen(
                    [str(c) for c in [*command, INPUTS['pack']]],
                    env=dict(env, **own),
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                )
            )
        states = planes = 0
        report = []
        for shard, run in enumerate(runs):
            stdout, stderr = run.communicate()
            summary = SUMMARY.search(stdout)
            self.assertIsNotNone(summary, f'shard {shard} exited {run.returncode}: {stdout[-2000:]}{stderr[-2000:]}')
            states += int(summary.group(1))
            planes += int(summary.group(3))
            if run.returncode:
                print('\n'.join(line for line in stdout.splitlines() if line.startswith('DIFFERS')))
                report.append(f'shard {shard}: {summary.group(0)}')
                report += [line for line in stdout.splitlines() if line.startswith('  ') and ' x ' in line]
        print(f'directcheck: {len(lines)} lines, {states} records made, {planes} of them two-plane forms')
        self.assertFalse(report, '\n'.join(report))
        self.assertGreater(planes, 0, 'no two-plane form was made')


if __name__ == '__main__':
    INPUTS.update(json.loads(Path(sys.argv[1]).read_text()))
    unittest.main(argv=sys.argv[:1])
