#!/usr/bin/env python3
"""Make one bucket of the driver's pipeline pack (drivers/TegraGM20BMetal/pack/directbucket.py).

    probe.py --generator <directbucket.py> --raw <dir> --qc default.metallib [--root <dir>] [--data-dir <dir>]
                [--data <metallib>]... --lines <bucket corpus> --out <dir named bucket<i>>
        one bucket's states and kernels made directly by the raw tools in <dir> (directpack beside the mock GM20B;
        bucket 0 also reads own.metallib and context.log, the channel's context init as nvdec ran it); their
        libraries' paths under --root (the crawl's root/ with this build's applications in it), else under
        QuartzCore's crawl, and the libraries --data and --data-dir name by their SHA-256

The bucket step alone, apart from make_pack.py's other steps: a bucket's action runs this file and directbucket.py and
nothing else of the generator, so an edit to the corpus, split, link or stamp reruns no bucket.
"""

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

# What the link reads of a bucket (directlink.py)
KEPT = ('records.bin', 'context.log')
CONTENT_NAME = re.compile(r'[0-9a-f]{64}\.metallib')


def fresh(path: Path) -> Path:
    """Return `path` as an empty directory."""
    shutil.rmtree(path, ignore_errors=True)
    path.mkdir(parents=True)
    return path


def content_name(library: Path) -> str:
    return hashlib.sha256(library.read_bytes()).hexdigest() + '.metallib'


def crawled_name(library: Path) -> str:
    if not CONTENT_NAME.fullmatch(library.name):
        raise ValueError(f'{library}: a --data-dir library is named <SHA-256>.metallib')
    return library.name


def library_view(libraries: list[Path], crawled: list[Path], destination: Path) -> Path:
    """Link every library into `destination` as <SHA-256>.metallib, the first of each name.

    `crawled` are a --data-dir's, which the crawl already names so (tools/gpu/shaders/crawl.py, inputs.py embedded).
    """
    destination.mkdir()
    named = [(content_name(source), source) for source in libraries]
    named += [(crawled_name(source), source) for source in crawled]
    for name, source in named:
        target = destination / name
        if not target.exists():
            target.symlink_to(source.resolve())
    return destination


def probe(args: argparse.Namespace, work: Path) -> None:
    out = fresh(Path(args.out).resolve())
    shutil.copyfile(args.lines, out / 'corpus.log')
    crawled = sorted(Path(args.data_dir).glob('*.metallib')) if args.data_dir else []
    data = library_view([Path(p) for p in args.data], crawled, work / 'data')
    arguments = ['--probe-bucket', out, '--prebuilt', Path(args.raw).resolve(), '--data-libs', data]
    arguments += ['--root', Path(args.root).resolve()] if args.root else []
    arguments += ['--qc', Path(args.qc).resolve()]
    arguments += ['--ca-selectors', Path(args.ca_selectors).resolve()] if args.ca_selectors else []
    env = {
        'PATH': '/usr/bin:/bin',
        'ZERO_AR_DATE': '1',
        'HOME': str(work / 'home'),
        'TMPDIR': os.environ.get('TMPDIR', str(work)),
    }
    command = [sys.executable, str(Path.cwd() / args.generator), *map(str, arguments)]
    result = subprocess.run(command, env=env, capture_output=True, text=True, check=False)
    if result.returncode:
        sys.exit(f'making {out.name} failed:\n{(result.stdout + result.stderr)[-6000:]}')
    print(result.stdout.rstrip())
    for path in out.iterdir():
        if path.is_file() and path.name not in KEPT:
            path.unlink()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ('generator', 'raw', 'qc', 'lines', 'out'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--root')
    parser.add_argument('--ca-selectors')
    parser.add_argument('--data-dir')
    parser.add_argument('--data', action='append', default=[])
    args = parser.parse_args()
    work = fresh(Path.cwd() / f'_probe_{os.getpid()}')
    (work / 'home').mkdir()
    probe(args, work)
    shutil.rmtree(work)


if __name__ == '__main__':
    main()
