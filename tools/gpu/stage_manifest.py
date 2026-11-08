#!/usr/bin/env python3
r"""List what deploy stages onto the Data volume (//gpu:bundle): the signed bundle, the shader seed and the raw pack.

usage: stage_manifest.py --self <rlocation> --base <dir> --base-rlocation R --seed <join's dir> --seed-rlocation R
                         --pack <bucket pack> --pack-rlocation R ... --raw <dir> --raw-rlocation R --out <manifest>

The manifest names files where Bazel left them, in //deploy:sd's runfiles, so nothing is copied to make it:

    # gpu stage 1 <its own rlocation path>
    <path below the Data volume>\t<sha256>\t<size>\t<mode, octal>\t<rlocation path of the file>

The stager (drivers/TegraGM20BMetal/stage-tegra-gm20b-metal.py) finds the runfiles root by taking the manifest's own
rlocation path off its location.
"""

import argparse
from pathlib import Path

from tools.common import hashing

HEADER = '# gpu stage 1 '
SEED = 'ngpu/addons/TegraGM20BMetal.m2v-seed/shared'
RAW = 'ngpu/addons/TegraGM20BMetal.raw'
EXECUTABLES = ('TegraGM20BMetal', 'libvulkan_nouveau.dylib')


def line(path: Path, digest: str, target: str, rlocation: str) -> str:
    """Return the manifest line of the file at `path`, staged at `target`."""
    mode = 0o755 if path.name in EXECUTABLES else 0o644
    return f'{target}\t{digest}\t{path.stat().st_size}\t{mode:o}\t{rlocation}'


def tree(directory: Path, rlocation: str, under: str) -> list[tuple[Path, str, str]]:
    """Return every file below `directory`, at `under` on the volume."""
    files = sorted(p for p in directory.rglob('*') if p.is_file())
    return [
        (p, under + p.relative_to(directory).as_posix(), rlocation + '/' + p.relative_to(directory).as_posix())
        for p in files
    ]


def main() -> None:
    """Write the manifest."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ('self', 'base', 'base-rlocation', 'seed', 'seed-rlocation', 'raw', 'raw-rlocation', 'out'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--pack', action='append', default=[])
    parser.add_argument('--pack-rlocation', action='append', default=[])
    args = parser.parse_args()
    files = tree(Path(args.base) / 'ngpu', args.base_rlocation + '/ngpu', 'ngpu/')
    files += tree(Path(args.seed), args.seed_rlocation, SEED + '/')
    files += [
        (Path(pack), f'{SEED}/packs/{number:03d}', rlocation)
        for number, (pack, rlocation) in enumerate(zip(args.pack, args.pack_rlocation, strict=True))
    ]
    files += tree(Path(args.raw), args.raw_rlocation, RAW + '/')
    digests = hashing.sha256_files([path for path, _, _ in files])
    lines = [line(path, digest, *where) for (path, *where), digest in zip(files, digests, strict=True)]
    Path(args.out).write_text(HEADER + args.self + '\n' + ''.join(entry + '\n' for entry in sorted(lines)))


if __name__ == '__main__':
    main()
