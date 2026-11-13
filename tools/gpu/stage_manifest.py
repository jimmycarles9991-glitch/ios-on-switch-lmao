#!/usr/bin/env python3
r"""List what deploy stages onto the Data volume (//gpu:bundle): the signed bundle, the shader seed and the raw pack.

usage: stage_manifest.py --self <rlocation> --base <dir> --base-rlocation R --seed <join's dir> --seed-rlocation R
                         --pack <bucket pack> --pack-rlocation R ... --raw <dir> --raw-rlocation R --out <manifest>
                         --native <packed native dir> --native-rlocation R
       stage_manifest.py --part --seed ... --pack ... --raw ... --native ... --out <part>
       stage_manifest.py --self <rlocation> --base <dir> --base-rlocation R --from-part <part> --out <manifest>

--part lists the seed's and the raw pack's files alone, and --from-part takes them from that list: a plugin edit then
hashes only the bundle and reads none of the seed's files.

The manifest names files where Bazel left them, in //deploy:sd's runfiles, so nothing is copied to make it:

    # gpu stage 1 <its own rlocation path>
    <path below the Data volume>\t<sha256>\t<size>\t<mode, octal>\t<rlocation path of the file>

The stager (drivers/TegraGM20BMetal/stage-tegra-gm20b-metal.py) finds the runfiles root by taking the manifest's own
rlocation path off its location.
"""

import argparse
from pathlib import Path

from tools.common import hashing
from tools.gpu import seed_pack

HEADER = '# gpu stage 1 '
SEED = 'ngpu/addons/TegraGM20BMetal.m2v-seed/shared'
RAW = 'ngpu/addons/TegraGM20BMetal.raw'
EXECUTABLES = ('TegraGM20BMetal', 'libvulkan_nouveau.dylib')


def line(path: Path, digest: str, target: str, rlocation: str) -> str:
    """Return the manifest line of the file at `path`, staged at `target`."""
    mode = 0o755 if path.name in EXECUTABLES else 0o644
    return f'{target}\t{digest}\t{path.stat().st_size}\t{mode:o}\t{rlocation}'


def native_target(native: Path, relative: str) -> str | None:
    """Return where a file of the packed native seed goes on the volume.

    None for the host's files beside it and the derived entries' packs.
    """
    top, _, below = relative.partition('/')
    if top == 'native-analysis':
        under = SEED
    elif top.startswith('raw-'):
        under = RAW + '/native'
    else:
        return None
    return under + '/' + relative if below and seed_pack.staged(native / top, below) else None


def tree(directory: Path, rlocation: str, under: str) -> list[tuple[Path, str, str]]:
    """Return every file below `directory`, at `under` on the volume."""
    files = sorted(p for p in directory.rglob('*') if p.is_file())
    return [
        (p, under + p.relative_to(directory).as_posix(), rlocation + '/' + p.relative_to(directory).as_posix())
        for p in files
    ]


def seed_files(args: argparse.Namespace) -> list[tuple[Path, str, str]]:
    """Return the seed's, the raw pack's and the native seed's files, each with where it goes and its rlocation."""
    files = tree(Path(args.seed), args.seed_rlocation, SEED + '/')
    files += [
        (Path(pack), f'{SEED}/packs/{number:03d}', rlocation)
        for number, (pack, rlocation) in enumerate(zip(args.pack, args.pack_rlocation, strict=True))
    ]
    files += tree(Path(args.raw), args.raw_rlocation, RAW + '/')
    files += [
        (path, target, rlocation)
        for path, relative, rlocation in tree(Path(args.native), args.native_rlocation, '')
        if (target := native_target(Path(args.native), relative))
    ]
    return files


def lines_of(files: list[tuple[Path, str, str]]) -> list[str]:
    digests = hashing.sha256_files([path for path, _, _ in files])
    return [line(path, digest, *where) for (path, *where), digest in zip(files, digests, strict=True)]


def main() -> None:
    """Write the manifest, or the seed's part of it."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ('self', 'base', 'base-rlocation', 'seed', 'seed-rlocation', 'raw', 'raw-rlocation', 'native'):
        parser.add_argument('--' + name)
    parser.add_argument('--native-rlocation')
    parser.add_argument('--out', required=True)
    parser.add_argument('--pack', action='append', default=[])
    parser.add_argument('--pack-rlocation', action='append', default=[])
    parser.add_argument('--part', action='store_true')
    parser.add_argument('--from-part')
    args = parser.parse_args()
    if args.part:
        Path(args.out).write_text(''.join(entry + '\n' for entry in sorted(lines_of(seed_files(args)))))
        return
    lines = lines_of(tree(Path(args.base) / 'ngpu', args.base_rlocation + '/ngpu', 'ngpu/'))
    lines += Path(args.from_part).read_text().splitlines() if args.from_part else lines_of(seed_files(args))
    Path(args.out).write_text(HEADER + args.self + '\n' + ''.join(entry + '\n' for entry in sorted(lines)))


if __name__ == '__main__':
    main()
