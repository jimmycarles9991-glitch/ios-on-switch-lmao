"""raw's frame held to eq's goldens: rawframe over the eq capture, its log against draws_<kind>.tsv (draw_goldens.py).

usage: eq_test.py <rawframe> <pack.bin> <kind> <testdata dir> [--qc27] <the pack's report files>...

The frame is the one the goldens were written from (the same corpus, arguments and FRAME_* settings); the pack's own
programs.bin and layouts.txt (its link's report group) name raw's programs and lay out their set 0 for nvdec.
"""

import gzip
import os
import subprocess
import sys
from pathlib import Path

import draw_goldens


def main() -> int:
    rawframe, pack, kind, testdata, *rest = sys.argv[1:]
    flags = [a for a in rest if a.startswith('--')]
    linked = next(Path(a) for a in rest if Path(a).name == 'linked')
    data, tmp = Path(testdata), Path(os.environ.get('TEST_TMPDIR', '/tmp'))
    kept = Path(os.environ.get('TEST_UNDECLARED_OUTPUTS_DIR', tmp))
    corpus = tmp / 'frame.corpus'
    corpus.write_bytes(gzip.decompress((data / f'frame_{kind}.corpus.gz').read_bytes()))
    env = dict(
        os.environ,
        RAWCAP_LOG=str(kept / 'raw.log'),
        RAWCAP_PROGRAMS=str(linked / 'programs.bin'),
        RAWCAP_LAYOUTS=str(linked / 'layouts.txt'),
        NVDEC_REGS='1',
        NVDEC_ROOTWORDS='1',
        FRAME_ALL_STATES='1',
        FRAME_STATES='2000',
    )
    if '--qc27' in flags:
        env['FRAME_QC27'] = '1'
    run = subprocess.run(
        [rawframe, pack, str(corpus), '1', '8', '2000'], env=env, capture_output=True, text=True, check=False
    )
    if run.returncode:
        print(f'rawframe exited {run.returncode}:\n{run.stderr[-3000:]}')
        return 1
    want = draw_goldens.read(str(data / f'draws_{kind}.tsv'))
    moved = draw_goldens.differences(want, str(kept / 'raw.log'))
    for line in moved[:40]:
        print(line)
    print(f'{sum(r[0] == "E" for r in want)} golden draws: {len(moved)} differences')
    return 1 if moved else 0


if __name__ == '__main__':
    sys.exit(main())
