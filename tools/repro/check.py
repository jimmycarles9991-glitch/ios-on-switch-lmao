#!/usr/bin/env python3
"""Reproducibility check: build the targets twice from scratch and compare every output file bit for bit.

The builds use two fresh output bases and --config=reproducible-check (no disk or remote cache).

    bazel run //tools/repro:check                      # //:kernel //:boot //:gpu
    bazel run //tools/repro:check -- //:kernel         # just these
    bazel run //tools/repro:check -- --keep            # keep the two output bases (under $TMPDIR) afterwards

Each build compiles everything (XNU, every driver, both Rust bridges): expect 10+ minutes and ~2 GB per output base
while it runs. Prints the "INFO: Elapsed / processes" lines of both builds and the comparison.
"""

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def outputs(bazel: str, base: str, workspace: str, targets: list[str], flags: list[str]) -> dict[str, str]:
    files = {}
    query = subprocess.run(
        [
            bazel,
            '--output_base=' + base,
            'cquery',
            *flags,
            '--output=files',
            ' + '.join(targets),
        ],
        cwd=workspace,
        capture_output=True,
        text=True,
        check=True,
    )
    exec_root = subprocess.run(
        [bazel, '--output_base=' + base, 'info', 'execution_root'],
        cwd=workspace,
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()
    for line in query.stdout.split():
        path = Path(exec_root, line)
        items = [p for p in path.rglob('*') if p.is_file()] if path.is_dir() else [path]
        for p in items:
            files[str(p.relative_to(exec_root))] = hashlib.sha256(p.read_bytes()).hexdigest()
    return files


def compare(a: dict[str, str], b: dict[str, str]) -> int:
    differ = sorted(k for k in set(a) | set(b) if a.get(k) != b.get(k))
    print(f'{len(a)} output files, {len(differ)} differ')
    for k in differ[:50]:
        print('  differs:', k)
    if not differ:
        for k, digest in sorted(a.items())[:50]:
            print(digest, k)
    return int(bool(differ))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('targets', nargs='*', default=['//:kernel', '//:boot', '//:gpu'])
    parser.add_argument('--keep', action='store_true')
    parser.add_argument('--remote-only', action='store_true', help='use remote workers for both cold builds')
    args = parser.parse_args()
    workspace = os.environ.get('BUILD_WORKSPACE_DIRECTORY') or sys.exit('run with bazel run')
    bazel = os.environ.get('BAZEL', 'bazel')
    scratch = Path(tempfile.mkdtemp(prefix='switch-repro-'))
    results = []
    flags = ['--config=reproducible-check', '--experimental_convenience_symlinks=ignore']
    if args.remote_only:
        flags += [
            '--config=agent',
            '--spawn_strategy=remote',
            '--strategy=AssetCatalogCompile=remote',
            '--noremote_local_fallback',
        ]
    try:
        for n in (1, 2):
            base = str(scratch / f'base{n}')
            build = subprocess.run(
                [
                    bazel,
                    '--output_base=' + base,
                    'build',
                    *flags,
                    '--execution_log_compact_file=' + str(scratch / f'exec{n}.zst'),
                    *args.targets,
                ],
                cwd=workspace,
                capture_output=True,
                text=True,
                check=False,
            )
            for line in build.stderr.splitlines():
                if line.startswith('INFO: Elapsed') or ' processes' in line:
                    print(f'build {n}: {line}')
            if build.returncode:
                sys.exit(build.stderr[-4000:])
            results.append(outputs(bazel, base, workspace, args.targets, flags))
            if not args.keep:
                subprocess.run([bazel, '--output_base=' + base, 'shutdown'], cwd=workspace, check=True)
                shutil.rmtree(base)
        sys.exit(compare(*results))
    finally:
        if not args.keep:
            for n in (1, 2):
                if (scratch / f'base{n}').exists():
                    subprocess.run(
                        [bazel, '--output_base=' + str(scratch / f'base{n}'), 'shutdown'],
                        cwd=workspace,
                        capture_output=True,
                        check=False,
                    )
            shutil.rmtree(scratch, ignore_errors=True)


if __name__ == '__main__':
    main()
