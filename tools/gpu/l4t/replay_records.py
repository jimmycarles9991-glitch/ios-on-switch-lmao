"""Rebuild the NAK records and Metal reflection for the five HW Test draws."""

import argparse
import os
import shutil
import subprocess
from pathlib import Path

from replay_payload import CASES


def capture(run: Path, case: str, out: Path, bin_dir: Path, root: Path) -> None:
    profile = run / 'profile' / case
    (reference,) = profile.glob('paired/*.json')
    source = out / 'substitute'
    source.mkdir(parents=True)
    for i in range(2):
        name = f'stage{i}.spv'
        shutil.copyfile(run / 'states' / case / 'translated/0000' / name, source / name)
        name = f'stage{i}.r32.spv'
        shutil.copyfile(profile / 'translated/0000' / name, source / name)
    env = dict(
        os.environ,
        PROBE_LIB_ROOT=str(root),
        DIRECTCHECK_DUMP=str(out),
        DIRECTCHECK_PIPELINES=str(out / 'stages'),
        DIRECTCHECK_REPLAY_DIR=str(out),
        DIRECTCHECK_STAGE_INPUTS=str(source),
    )
    command = [
        bin_dir / 'drivers/TegraGM20BMetal/test/directcheck/directcheck',
        root / 'System/Library/Frameworks/QuartzCore.framework/default.metallib',
        profile / 'corpus.log',
        bin_dir / 'gpu/seed/24B119/pack/pack.bin',
    ]
    log = out / 'build.log'
    with log.open('xb') as f:
        status = subprocess.run(command, env=env, stdout=f, stderr=subprocess.STDOUT, check=False).returncode
    files = [out / (reference.stem + ext) for ext in ('.json', '.rec', '.reflection')]
    if status not in (0, 1) or not all(p.is_file() and p.stat().st_size for p in files):
        raise RuntimeError(f'{case}: directcheck failed ({status}), see {log}')
    print(reference.stem, files[-1].stat().st_size, 'reflection bytes')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    parser.add_argument('destination', type=Path)
    parser.add_argument('--bazel-bin', type=Path, required=True)
    parser.add_argument('--library-root', type=Path, required=True)
    args = parser.parse_args()
    out = args.destination.resolve()
    if out.is_relative_to(Path(__file__).resolve().parents[3]):
        parser.error('choose a fresh destination outside the checkout')
    out.mkdir()
    for case in CASES:
        capture(args.run.resolve(), case, out / case, args.bazel_bin.resolve(), args.library_root.resolve())


if __name__ == '__main__':
    main()
