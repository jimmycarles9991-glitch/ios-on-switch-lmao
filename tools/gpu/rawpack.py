#!/usr/bin/env python3
"""Make the raw GM20B path's pipeline pack (drivers/TegraGM20BMetal/raw/pack/rawpack.py) beside the seed.

One action per step (gpu/seed/tools.bzl seed_tools builds, gpu/seed/defs.bzl seed_raw_pack runs the rest):

    rawpack.py build --xcode-env F --host-bridge <darwin bridge .a> --generator <raw/pack/rawpack.py> --tree <tree>
                     --out <dir>
        the generator's host tools (directpack and the probes through this build's frontend and host bridge, the mock
        GM20B, the raw path's own.metallib) into <dir>
    rawpack.py corpus --corpus corpus.log [--archives <build>.tsv] --out <file>
        the corpus lines the probe makes (raw/pack/probe.mm: Core Animation's render states, without lib=), so an edit
        to any other line leaves the run's input as it was; with the render states Apple's archives declare for the
        device (precompile/archives/<build>.tsv) whose functions are one framework's default.metallib
    rawpack.py split --generator <rawpack.py> --qc default.metallib --corpus <file> --out <bucket corpus>...
        the states of each of the generator's buckets
    rawpack.py probe --generator <rawpack.py> --raw <dir> --nvk <twin dylib> --qc default.metallib
                     --lines <bucket corpus> --out <dir named bucket<i>>
        one bucket's states made directly through the shader tools (raw/ holds directpack, the probes, the mock and
        own.metallib; the NVK twin), with bucket 0's warm-up draw and the bucket's kernels probed over the mock GM20B
    rawpack.py link --generator <rawpack.py> --out <dir> <bucket dir>...
        the pack made from the buckets' records, unstamped: <out>/pack.bin and report.txt
    rawpack.py stamp --generator <rawpack.py> --pack <pack.bin> --manifest <MANIFEST.json> --out <dir>
        <out>/pack.bin with the tools' stack key in its header

Only the stamp reads the stack key, which every raw/ source changes: a raw runtime edit reruns the stamp alone (but for
the direct compile's own sources, which the tools build from), and a bucket runs again only when its states, the tools,
the NVK twin or QuartzCore's library change. No step but build
runs a compiler, so the image builder runs them with the release's prebuilt tools.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

PROBE_LABEL = ' label=com.apple.coreanimation.'
LIBRARY = re.compile(r' lib=\S+')
# A function a state links (" | <vl|vp|fl|fp> <function> lib=<library> ..."), which any library may hold
LINKED_LIBRARY = re.compile(r'( \| [vf][lp] \S+) lib=\S+')
# What the link reads of a bucket (directlink.py)
KEPT = ('records.bin', 'warm.out', 'warm.log', 'programs.bin', 'compute.out', 'compute.log')
# The frameworks other than QuartzCore whose states the pack holds: their lines keep lib=, which the probe loads
# them from
FRAMEWORKS = ('RenderBox',)
FRAMEWORK_LIBRARY = re.compile(r' lib=/System/Library/(?:Private)?Frameworks/(\w+)\.framework/default\.metallib')


def framework_line(line: str) -> bool:
    """Whether the line's stages are both one pack framework's default.metallib (what they link may be another's)."""
    libs = set(LIBRARY.findall(LINKED_LIBRARY.sub(r'\1', line)))
    return len(libs) == 1 and any(
        lib == f' lib=/System/Library/PrivateFrameworks/{name}.framework/default.metallib'
        for lib in libs
        for name in FRAMEWORKS
    )


def probe_lines(corpus: str) -> list[str]:
    """Return the corpus lines probe.mm and probecompute.mm make, as they read them (a D line keeps its libraries)."""
    return [
        line if framework_line(line) or line.startswith('D ') else LIBRARY.sub('', line)
        for line in corpus.split('\n')
        if (line.startswith('R ') and ' ok=1 ' in line and (PROBE_LABEL in line or framework_line(line)))
        or line.startswith('D ok=1 ')
    ]


def archive_lines(inventory: str) -> list[str]:
    """Return the render states an archive inventory (corpus/tools/archives/declared.py) has the pack make.

    Those declared for the raw device's GPU families that the runtime looks up in the pack (RawMetalStates.mm
    _packable: a vertex and a fragment function, both one framework's default.metallib, and so is every function it
    links); not QuartzCore's, whose archive is its offline compilation manifest (raw/pack/ca/caset.py's manifest tier).
    They keep lib=, which the probe loads them from.
    """
    out = []
    for row in inventory.splitlines():
        kind, _owner, _source, family, line = row.split('\t')
        libraries = set(LIBRARY.findall(line))
        frameworks = set(FRAMEWORK_LIBRARY.findall(line))
        both = ' | v - ' not in line and ' | f - ' not in line
        if kind == 'render' and family == 'device' and both and len(libraries) == len(frameworks) == 1:
            out += [line] if frameworks != {'QuartzCore'} else []
    return out


def generate(generator: list[str], arguments: list[str | Path], env: dict[str, str], what: str) -> str:
    """Run the generator; exit with its output's tail if it fails."""
    result = subprocess.run([*generator, *map(str, arguments)], env=env, capture_output=True, text=True, check=False)
    if result.returncode:
        sys.exit(f'{what} failed:\n{(result.stdout + result.stderr)[-6000:]}')
    return result.stdout


def fresh(path: Path) -> Path:
    """Return `path` as an empty directory."""
    shutil.rmtree(path, ignore_errors=True)
    path.mkdir(parents=True)
    return path


def build(args: argparse.Namespace, generator: list[str], env: dict[str, str], root: Path) -> None:
    xcode = dict(line.split('=', 1) for line in Path(args.xcode_env).read_text().splitlines() if '=' in line)
    env['DEVELOPER_DIR'] = xcode['DEVELOPER_DIR']
    arguments = ['--build-only', '--bridge', root / args.host_bridge, '--metal-abi', root / args.metal_abi]
    arguments += ['--out', root / args.out]
    generate(generator, arguments, env, "the pack generator's tools")


def split(args: argparse.Namespace, generator: list[str], env: dict[str, str], work: Path) -> None:
    arguments = ['--split', '--qc', Path(args.qc).resolve(), '--corpus', Path(args.corpus).resolve(), '--out', work]
    print(generate(generator, arguments, env, 'the split into buckets').rstrip())
    made = sorted((work / 'work').glob('bucket*'), key=lambda d: int(d.name.removeprefix('bucket')))
    if len(made) != len(args.out):
        sys.exit(f'the generator made {len(made)} buckets, the build declares {len(args.out)}')
    for bucket, out in zip(made, args.out, strict=True):
        shutil.copyfile(bucket / 'corpus.log', out)


def probe(args: argparse.Namespace, generator: list[str], env: dict[str, str], work: Path) -> None:
    out = fresh(Path(args.out).resolve())
    shutil.copyfile(args.lines, out / 'corpus.log')
    # The probe opens NVK from a copy of its own: an input is a link to the output base's file, which dyld can name by
    # another of its links.
    dylib = work / 'libvulkan_nouveau.dylib'
    shutil.copyfile(args.nvk, dylib)
    dylib.chmod(0o755)
    arguments = ['--probe-bucket', out, '--prebuilt', Path(args.raw).resolve(), '--nvk', dylib]
    print(generate(generator, [*arguments, '--qc', Path(args.qc).resolve()], env, f'making {out.name}').rstrip())
    for path in out.iterdir():
        if path.is_file() and path.name not in KEPT:
            path.unlink()


def link(args: argparse.Namespace, generator: list[str], env: dict[str, str], work: Path) -> None:
    linked = fresh(work / 'linked')
    report = generate(generator, ['--link', '--out', linked, *[Path(b).resolve() for b in args.buckets]], env, 'link')
    # Every state left out has its own line, megabytes of them: Bazel shows none of an action's output past 1 MiB, so
    # the summary comes here and the lines stay in report.txt (the link's output group).
    skipped = [line for line in report.splitlines() if line.startswith('  skipped')]
    print('\n'.join(line for line in report.splitlines() if not line.startswith('  skipped')))
    print(f'{len(skipped)} states left out, each named in report.txt')
    out = fresh(Path(args.out))
    shutil.copyfile(linked / 'pack.bin', out / 'pack.bin')
    shutil.copyfile(linked / 'report.txt', out / 'report.txt')


def stamp(args: argparse.Namespace, generator: list[str], env: dict[str, str], _work: Path) -> None:
    stack = json.loads(Path(args.manifest).read_text())['stack']
    out = fresh(Path(args.out))
    shutil.copyfile(args.pack, out / 'pack.bin')
    generate(generator, ['--stamp', out / 'pack.bin', '--stack', stack], env, 'the stamp')
    print(f'raw pack: stack {stack}')


STEPS = {
    'build': ('xcode-env', 'host-bridge', 'metal-abi', 'generator', 'tree', 'out'),
    'split': ('generator', 'qc', 'corpus'),
    'probe': ('generator', 'raw', 'nvk', 'qc', 'lines', 'out'),
    'link': ('generator', 'out'),
    'stamp': ('generator', 'pack', 'manifest', 'out'),
}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    steps = parser.add_subparsers(dest='step', required=True)
    corpus = steps.add_parser('corpus')
    for name in ('corpus', 'out'):
        corpus.add_argument('--' + name, required=True)
    corpus.add_argument('--archives')
    for step, names in STEPS.items():
        p = steps.add_parser(step)
        for name in names:
            p.add_argument('--' + name, required=True)
    steps.choices['split'].add_argument('--out', nargs='+', required=True)
    steps.choices['link'].add_argument('buckets', nargs='+')
    args = parser.parse_args()
    if args.step == 'corpus':
        lines = probe_lines(Path(args.corpus).read_text())
        if args.archives:
            harvested = set(lines)
            lines += [line for line in archive_lines(Path(args.archives).read_text()) if line not in harvested]
        Path(args.out).write_text(''.join(line + '\n' for line in lines))
        return
    root = Path.cwd()
    work = fresh(root / f'_rawpack_{args.step}_{os.getpid()}')
    (work / 'home').mkdir()
    env = {
        'PATH': '/usr/bin:/bin',
        'ZERO_AR_DATE': '1',
        'HOME': str(work / 'home'),
        'TMPDIR': os.environ.get('TMPDIR', str(work)),
        'RAWPACK_TREE': str(root / getattr(args, 'tree', '.')),
    }
    generator = [sys.executable, str(root / args.generator)]
    {'build': build, 'split': split, 'probe': probe, 'link': link, 'stamp': stamp}[args.step](
        args, generator, env, root if args.step == 'build' else work
    )
    shutil.rmtree(work)


if __name__ == '__main__':
    main()
