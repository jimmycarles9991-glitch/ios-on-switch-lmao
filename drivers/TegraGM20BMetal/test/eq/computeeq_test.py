"""Each recorded kernel's dispatch through raw's compute encoder held to computeeq's goldens (compute_goldens.py).

usage: computeeq_test.py <computeframe> <pack.bin> <kind> <testdata dir>

testdata/compute_<kind>/<kernel>/ holds the NVK run's record of the kernel (its K lines, the threadgroup first) and
NVK's program, which computeframe's raw mode places and dispatches over a 2 x 2 grid as the NVK run did.
"""

import os
import subprocess
import sys
from pathlib import Path

import compute_goldens


def main() -> int:
    computeframe, pack, kind, testdata = sys.argv[1:5]
    kernels = Path(testdata) / f'compute_{kind}'
    kept = Path(os.environ.get('TEST_UNDECLARED_OUTPUTS_DIR', os.environ.get('TEST_TMPDIR', '/tmp')))
    golden = str(Path(testdata) / f'compute_{kind}.tsv')
    moved, runs = [], 0
    for kernel in sorted(p for p in kernels.iterdir() if p.is_dir()):
        record = kernel / 'record.k'
        tg = record.read_text().splitlines()[0].removeprefix('# tg=')
        log = kept / f'{kernel.name}.log'
        env = dict(os.environ, RAWCAP_LOG=str(log), RAWCAP_PROGRAMS=str(kernel / 'program.bin'), NVDEC_REGS='1')
        args = [computeframe, 'raw', pack, str(record), str(kernel / 'program.bin'), tg, '2,2,1']
        run = subprocess.run(args, env=env, capture_output=True, text=True, check=False)
        runs += 1
        if run.returncode:
            moved.append(f'{kernel.name}: computeframe exited {run.returncode}: {run.stderr[-300:]}')
            continue
        moved += compute_goldens.check(str(log), kernel.name, golden)
    for line in moved:
        print(line)
    print(f'{runs} kernels: {len(moved)} differences')
    return 1 if moved or not runs else 0


if __name__ == '__main__':
    sys.exit(main())
