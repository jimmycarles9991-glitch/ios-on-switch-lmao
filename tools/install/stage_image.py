#!/usr/bin/env python3
"""Merge one Bazel-packed install image (//install:packed_image) into //:image's folder (out/sd).

    bazel run //:image [--config=ios27] [-- --out DIR]

Parts are APFS-cloned (cp -c) when source and destination share a volume so staging adds no extra blocks, and made
writable (0644) so a later `//:image_boot` can refresh NAME.SII.001's head in place. Any other iOS version in the
folder stays untouched.
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(os.path.abspath(__file__)).parents[2]))
from tools.common import paths
from tools.common.files import replacing
from tools.install import folder, host


def _clone_or_copy(source: Path, target: Path) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    with replacing(target) as partial:
        partial.unlink(missing_ok=True)
        cloned = subprocess.run(['/bin/cp', '-c', str(source), str(partial)], capture_output=True, check=False)
        if cloned.returncode != 0:
            shutil.copyfile(source, partial)
        partial.chmod(0o644)


def _sync_tree(source: Path, target: Path) -> None:
    target.mkdir(parents=True, exist_ok=True)
    for item in sorted(source.rglob('*')):
        dest = target / item.relative_to(source)
        if item.is_dir():
            dest.mkdir(parents=True, exist_ok=True)
            dest.chmod(0o755)
        else:
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(item, dest)
            dest.chmod(0o644)


def stage(packed: Path, out: Path, log: host.Log) -> str:
    """Merge the single packed image in `packed` into `out`; return its image name (IOS17, IOS27)."""
    found = folder.records(packed)
    if len(found) != 1:
        raise host.HostError(f'{packed}: expected one image record, found {sorted(found)}')
    name, entry = next(iter(found.items()))
    out = folder.prepare(out)
    source_parts = sorted((packed / folder.SII_DIR).glob(f'{name}.SII.*'))
    if not source_parts:
        raise host.HostError(f'{packed}: holds no {name}.SII.* parts')
    keep = {p.name for p in source_parts}
    for existing in sorted((out / folder.SII_DIR).glob(f'{name}.SII.*')):
        if existing.name not in keep:
            existing.unlink()
    for part in source_parts:
        _clone_or_copy(part, out / folder.SII_DIR / part.name)
    for relative in folder.SHARED:
        src = packed / relative
        if src.is_file():
            folder.add(out, relative, src)
    pretranslated = packed / 'pretranslated'
    if pretranslated.is_dir():
        _sync_tree(pretranslated, out / 'pretranslated')
    folder.record(out, name, entry)
    minimum = entry.get('minimum_bytes')
    if minimum:
        log(f'folder: {out}; {name} needs a partition of {minimum / (1 << 30):.1f} GiB or more')
    else:
        log(f'folder: {out}; staged {name}')
    return name


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--packed', required=True, help='runfiles path or directory of //install:packed_image')
    parser.add_argument('--out', help='the folder to write (default: out/sd in the source tree)')
    parser.add_argument('--log')
    args = parser.parse_args(argv)
    packed = Path(paths.located(args.packed) or '')
    out = Path(os.path.abspath(args.out or paths.workspace() / 'out/sd'))
    log = host.Log(args.log or paths.run_log('image'))
    try:
        stage(packed, out, log)
        return 0
    except host.HostError as error:
        log('FAILED:', error)
        return 1
    finally:
        log.quiet('log:', log.path)


if __name__ == '__main__':
    sys.exit(main())
