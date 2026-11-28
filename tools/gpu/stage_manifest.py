#!/usr/bin/env python3
r"""List what deploy stages onto the Data volume (//gpu:bundle) and the preferences merged beside it.

The signed bundle, the shader seed and the pack, in tools/gpu/stage_format.py's format.

usage: stage_manifest.py --self <rlocation> --base <dir> --base-rlocation R --seed <replies' dir> --seed-rlocation R
                         --raw <dir> --raw-rlocation R --native <packed native dir> --native-rlocation R
                         --out <manifest>
       stage_manifest.py --part --seed ... --raw ... --native ... --out <part>
       stage_manifest.py --self <rlocation> --base <dir> --base-rlocation R --from-part <part> --out <manifest>
       stage_manifest.py --tree <manifest> --root <rlocation>=<dir> ... --out <dir>

--part lists the seed's and the pack's files alone, and --from-part takes them from that list: a plugin edit then
hashes only the bundle and reads none of the seed's files.

The manifest names files where Bazel left them, in //deploy:sd's runfiles, so nothing is copied to make it. --tree
copies them out into a stage tree (a release asset, the image builder's input): each file at its path below the Data
volume and the same records in <out>/gpu.stage. Each --root says where the files below an rlocation path are.
"""

import argparse
import shutil
from pathlib import Path

from tools.common import hashing
from tools.gpu import seed_pack, stage_format

SEED = 'ngpu/addons/TegraGM20BMetal.seed/shared'
RAW = 'ngpu/addons/TegraGM20BMetal.pack'
EXECUTABLES = ('TegraGM20BMetal',)


def record(path: Path, digest: str, target: str, rlocation: str) -> str:
    """Return the file record of the file at `path`, staged at `target`."""
    mode = 0o755 if path.name in EXECUTABLES else 0o644
    return stage_format.file_record(target, digest, path.stat().st_size, mode, rlocation)


def native_target(native: Path, relative: str) -> str | None:
    """Return where a file of the packed native seed goes on the volume.

    None for the host's files beside it and the derived entries' packs.
    """
    top, _, below = relative.partition('/')
    if not top.startswith('rec-') or not below or not seed_pack.staged(native / top, below):
        return None
    return RAW + '/native/' + relative


def tree(directory: Path, rlocation: str, under: str) -> list[tuple[Path, str, str]]:
    """Return every file below `directory`, at `under` on the volume."""
    files = sorted(p for p in directory.rglob('*') if p.is_file())
    return [
        (p, under + p.relative_to(directory).as_posix(), rlocation + '/' + p.relative_to(directory).as_posix())
        for p in files
    ]


def seed_files(args: argparse.Namespace) -> list[tuple[Path, str, str]]:
    """Return the seed's, the pack's and the native seed's files, each with where it goes and its rlocation."""
    files = tree(Path(args.seed), args.seed_rlocation, SEED + '/')
    files += tree(Path(args.raw), args.raw_rlocation, RAW + '/')
    files += [
        (path, target, rlocation)
        for path, relative, rlocation in tree(Path(args.native), args.native_rlocation, '')
        if (target := native_target(Path(args.native), relative))
    ]
    return files


def records_of(files: list[tuple[Path, str, str]]) -> list[str]:
    digests = hashing.sha256_files([path for path, _, _ in files])
    return [record(path, digest, *where) for (path, *where), digest in zip(files, digests, strict=True)]


def write_tree(manifest: Path, roots: list[str], out: Path) -> None:
    """Copy the files a runfiles manifest names into a stage tree at `out`, with the tree's own manifest."""
    lines = manifest.read_text().splitlines()
    where = dict(root.split('=', 1) for root in roots)
    out.mkdir(parents=True, exist_ok=True)
    for path, mode, source in stage_format.files(lines):
        top = next((r for r in where if source.startswith(r + '/')), None)
        if top is None:
            raise SystemExit(f'stage_manifest: no --root for {source}')
        relative = stage_format.check_path(source[len(top) + 1 :])
        target = out / stage_format.check_path(path)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(Path(where[top]) / relative, target)
        target.chmod(mode)
    tree_manifest = out / stage_format.TREE_MANIFEST
    tree_manifest.write_text(''.join(line + '\n' for line in stage_format.as_tree(lines)))
    tree_manifest.chmod(0o644)


def main() -> None:
    """Write the manifest, the seed's part of it, or a stage tree."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ('self', 'base', 'base-rlocation', 'seed', 'seed-rlocation', 'raw', 'raw-rlocation', 'native'):
        parser.add_argument('--' + name)
    parser.add_argument('--native-rlocation')
    parser.add_argument('--out', required=True)
    parser.add_argument('--part', action='store_true')
    parser.add_argument('--from-part')
    parser.add_argument('--kext-digest', help="a file holding the GPU kext's build identity")
    parser.add_argument('--tree')
    parser.add_argument('--root', action='append', default=[])
    args = parser.parse_args()
    if args.tree:
        write_tree(Path(args.tree), args.root, Path(args.out))
        return
    if args.part:
        Path(args.out).write_text(''.join(entry + '\n' for entry in sorted(records_of(seed_files(args)))))
        return
    files = records_of(tree(Path(args.base) / 'ngpu', args.base_rlocation + '/ngpu', 'ngpu/'))
    files += Path(args.from_part).read_text().splitlines() if args.from_part else records_of(seed_files(args))
    header = [stage_format.runfiles_header(args.self)]
    if args.kext_digest:
        header.append(stage_format.kext_header(Path(args.kext_digest).read_text().strip()))
    lines = [*header, *sorted(files), *stage_format.pref_records()]
    Path(args.out).write_text(''.join(line + '\n' for line in lines))


if __name__ == '__main__':
    main()
