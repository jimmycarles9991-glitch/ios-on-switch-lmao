"""snapshot.py --boot DIR --gpu MANIFEST --gpu-base DIR NAME: the boot entry and Metal bundle, as <state>/lkg/<NAME>.

<state> is ~/Library/Application Support/switch-ios ($SWITCH_HOME overrides it), outside every checkout (switchroot/,
bootloader/, overlay/ and gpu/). The snapshot is for rollback and for deploying exactly that build later:

    bazel run //boot:snapshot -- 1900-r9-something
    LKG="$HOME/Library/Application Support/switch-ios/lkg/1900-r9-something"
    bazel run //deploy:sd -- disk4 --yes --stage-gpu --boot-package="$LKG" --gpu-bundle="$LKG/gpu"

The snapshot also keeps the build's kernel.macho, which names the unslid kernel addresses in panics and in the
"SwitchKprof" lines: `xcrun atos -o "$LKG/kernel.macho" -arch arm64 <addr>...`.

Refuses to overwrite an existing snapshot.
"""

import argparse
import hashlib
import os
import shutil
import sys
from pathlib import Path

from tools.common import paths


def materialize(manifest: Path, out: Path) -> None:
    """Copy what //gpu:bundle's manifest names (tools/gpu/stage_manifest.py) below `out`, as the stager lays it out."""
    lines = manifest.read_text().splitlines()
    root = str(manifest)[: -len(lines[0].split(' ')[-1])]
    for line in lines[1:]:
        path, _, _, _, source = line.split('\t')
        if not (out / path).exists():
            (out / path).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(root + source, out / path)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--boot', required=True)
    parser.add_argument('--gpu', help="//gpu:bundle's manifest (the seed and the raw pack beside the bundle)")
    parser.add_argument('--gpu-base', help='//gpu:base: the bundle and its trust cache')
    parser.add_argument('--kernel', help="the build's kernel.macho, kept for symbolication")
    parser.add_argument('--no-gpu', action='store_true', help='snapshot the boot entry only')
    parser.add_argument('name')
    args = parser.parse_args()
    if '/' in args.name or args.name.startswith('.'):
        sys.exit('snapshot name must be a plain directory name')
    target = paths.state_dir() / 'lkg' / args.name
    if target.exists():
        sys.exit(f'{target} exists: pick another name')
    boot = paths.located(args.boot)
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(boot, target, symlinks=False)
    # copytree keeps bazel-bin's read-only modes; the snapshot root must take gpu/ and kernel.macho.
    target.chmod(0o755)
    if args.gpu and not args.no_gpu:
        shutil.copytree(paths.located(args.gpu_base), target / 'gpu', symlinks=False)
        for path in [target / 'gpu', *(target / 'gpu').rglob('*')]:
            if path.is_dir():
                path.chmod(0o755)
        # Located, not resolved: the manifest names the runfiles root from where it sits in the tree.
        materialize(Path(paths.located(args.gpu)), target / 'gpu')
    if args.kernel:
        shutil.copyfile(paths.located(args.kernel), target / 'kernel.macho')
    for path in target.rglob('*'):
        path.chmod(0o755 if path.is_dir() or os.access(path, os.X_OK) else 0o644)
    print(target)
    for path in sorted((target / 'switchroot/ios').iterdir()):
        print(f'  {path.name} sha256 {hashlib.sha256(path.read_bytes()).hexdigest()}')


if __name__ == '__main__':
    main()
