r"""Make one bucket of the seed: its lines through the prebuilt precompile tool, into an empty cache directory.

usage: precompile.py --precompile <tool> --mock <libmock_iokit.dylib> --nvk <twin libvulkan_nouveau.dylib>
                     --root <library root> --store <store.json> [--embedded <header>]... --qc <metallib>
                     --lines <bucket.log> [--seed <meta dir> --seed-pack <pack>...] [--guess-dispatch]
                     [--unused-list <file> --unused <input>...] --out <pack> --index <file> --report <file>

The bucket's lines after its `# derived` line are guesses: a refusal of one is counted, and a crash in one costs that
line only. The tool is run again past the line it died in (its PRECOMPILE_PROGRESS file says which), on the same cache
directory, so what it made before the crash is read back, not made again. Writes the pack (every file the cache
directory made, one after the other), the index (`<path below the cache directory>\t<sha256>\t<size>\t<offset in the
pack>` per file) and the report (what was refused and crashed, the driver records' identity).
--seed: the lines are made over the packed seed (merge.py join's directory and the buckets' packs in order), as the
device reads it; the report's `rates` says how many were found there.
--guess-dispatch: every compute state no D line names is dispatched at the 2D shape Apple's documentation computes.
--unused-list: written with the --unused inputs, the action's unused inputs list (gpu/seed/defs.bzl: the NVK twin, which
the action is keyed on by its compiler identity instead).
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

DERIVED = '# derived'
KINDS = 'SRCD'
CRASHES = 64
TIMEOUT = 3600
CONTENT = '529a20fd270ababa3e5ec2fe4d1674d93739ba5bcd61861674b4fb6db349af9f'
TIMING = re.compile(r' in [0-9.]+ ms')
SUMMARIES = ('precompile:', 'rates:')


def store_view(store: Path, root: Path, by_sha: dict[str, str], embedded: list[Path]) -> None:
    """Lay out the library store precompile.mm reads: root/ and data/<sha256>.metallib."""
    (store / 'data').mkdir(parents=True)
    (store / 'root').symlink_to(root.absolute())
    for digest, path in by_sha.items():
        (store / 'data' / (digest + '.metallib')).symlink_to(Path(path).absolute())
    for header in embedded:
        text = header.read_text()
        library = bytes(int(b, 16) for b in re.findall(r'0x([0-9a-fA-F]{2})', text[text.index('{') : text.rindex('}')]))
        (store / 'data' / (hashlib.sha256(library).hexdigest() + '.metallib')).write_bytes(library)


def place(source: Path, target: Path) -> None:
    """Put a tool where the run expects it: a hard link to the input, a copy where the file system refuses one."""
    try:
        os.link(os.path.realpath(source), target)
    except OSError:
        shutil.copyfile(source, target)
        target.chmod(0o755)


def seed_view(meta: Path, packs: list[Path], view: Path) -> None:
    """Lay out the packed seed as the device holds it: the join's files, and packs/<NNN> the buckets' packs."""
    (view / 'packs').mkdir(parents=True)
    for path in meta.rglob('*'):
        if path.is_file():
            (view / path.relative_to(meta)).symlink_to(path.absolute())
    for number, pack in enumerate(packs):
        (view / 'packs' / f'{number:03d}').symlink_to(pack.absolute())


def by_kind(lines: list[str]) -> dict[str, list[str]]:
    """Return the lines of each kind, in the order precompile.mm makes them."""
    return {kind: [line for line in lines if line.startswith(kind + ' ')] for kind in KINDS}


def remaining(corpus: dict[str, list[str]], derived: dict[str, list[str]], kind: str, index: int) -> tuple:
    """Split what is left after the line `index` of `kind` (corpus lines first, as the tool reads them) died."""
    order = KINDS.index(kind)
    left_corpus, left_derived = {}, {}
    for k in KINDS:
        both = corpus[k] + derived[k]
        start = 0 if KINDS.index(k) > order else index + 1 if k == kind else len(both)
        left_corpus[k] = [line for i, line in enumerate(both) if i >= start and i < len(corpus[k])]
        left_derived[k] = [line for i, line in enumerate(both) if i >= start and i >= len(corpus[k])]
    return left_corpus, left_derived


def _journal(path: Path, lines: dict[str, list[str]]) -> Path:
    path.write_text(''.join(line + '\n' for k in KINDS for line in lines[k]))
    return path


RATES = re.compile(r'(cache|drv|pso)=(\S+)')


def rates(summaries: list[str]) -> dict[str, str]:
    """Return the cache, driver and pipeline-state counts of the tool's last rates line."""
    lines = [line for line in summaries if line.startswith('rates:')]
    return dict(RATES.findall(lines[-1])) if lines else {}


def _died_at(progress: Path, corpus: dict, derived: dict, output: str) -> tuple[str, int]:
    """Return the line the tool was making when it died, after checking it read the lines as they were given."""
    words = progress.read_text().split() if progress.exists() else []
    if len(words) < 7 or words[0] != 'lines':
        sys.exit(f'precompile died before its first line:\n{output[-4000:]}')
    given = [len(corpus[k]) + len(derived[k]) for k in KINDS]
    if [int(n) for n in words[1:5]] != given:
        sys.exit(f'precompile read {words[1:5]} lines by kind, not the {given} given (two lines it takes as one)')
    return words[-2], int(words[-1])


def failure(output: str) -> str:
    """Say why the tool exited non-zero: the corpus states of QuartzCore's library it refused, or its output's tail."""
    refused = sorted({line.split(' ', 1)[1] for line in output.splitlines() if line.startswith('REFUSED-CORPUS ')})
    if not refused:
        return '\n' + output[-6000:]
    listed = '\n'.join(refused)
    return f"{len(refused)} corpus state(s) of QuartzCore's library refused, which the device refuses too:\n{listed}"


def make(work: Path, args: argparse.Namespace, corpus: dict, derived: dict) -> dict:
    """Run the tool until every line is made or skipped; return what happened."""
    env = {
        'PATH': '/usr/bin:/bin',
        # work/precompile is a hard link, and macOS may name the executable by its other link (the tool's output
        # directory, without the mock beside it) when dyld resolves @rpath; the leaf-name search here does not depend
        # on which name the kernel picks.
        'DYLD_LIBRARY_PATH': str(work),
        'HOME': str(work / 'home'),
        'TMPDIR': str(work / 'tmp'),
        'MESA_SHADER_CACHE_DISABLE': 'true',
        'PRECOMPILE_PROGRESS': str(work / 'progress'),
    }
    if args.seed:
        seed_view(args.seed, args.seed_pack, work / 'seed')
        env['METAL2VULKAN_SHADER_CACHE_SEED'] = str(work / 'seed')
    (work / 'home').mkdir()
    (work / 'tmp').mkdir()
    crashed, summaries, refusals = [], [], []
    while any(corpus.values()) or any(derived.values()):
        command = [
            work / 'precompile',
            '--libs',
            work / 'store',
            work / 'libvulkan_nouveau.dylib',
            work / 'cache',
            args.qc,
            _journal(work / 'corpus.log', corpus),
            '--derived',
            _journal(work / 'derived.log', derived),
            *(['--guess-dispatch'] if args.guess_dispatch else []),
        ]
        try:
            result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=TIMEOUT, check=False)
            died, output = result.returncode < 0, result.stdout + result.stderr
        except subprocess.TimeoutExpired as expired:
            died, output = True, str(expired.stdout or '') + str(expired.stderr or '')
        # The report holds no path of this run's directory and no timing: a bucket made again, here or on another
        # machine, reports the same bytes.
        output = output.replace(str(work) + '/', '')
        refusals += [line.split(' ', 1)[1] for line in output.splitlines() if line.startswith('REFUSED')]
        summaries += [TIMING.sub('', line) for line in output.splitlines() if line.startswith(SUMMARIES)]
        if not died:
            if result.returncode:
                sys.exit(f'precompile failed ({result.returncode}): {failure(output)}')
            break
        kind, index = _died_at(work / 'progress', corpus, derived, output)
        line = (corpus[kind] + derived[kind])[index]
        if index < len(corpus[kind]) or len(crashed) >= CRASHES:
            sys.exit(f'precompile died making {kind} line {index}: {line[:400]}\n{output[-4000:]}')
        crashed.append(line)
        corpus, derived = remaining(corpus, derived, kind, index)
    return {'crashed': crashed, 'refused': refusals, 'runs': summaries, 'rates': rates(summaries)}


def pack(cache: Path, out: Path) -> tuple[list[str], str | None]:
    """Write every file of the cache directory into the pack `out`; return its index lines and the records' identity."""
    lines, identity, offset = [], None, 0
    layout = cache / 'v1'
    files = (
        sorted(p for p in layout.rglob('*') if p.is_file() and not p.name.startswith('.')) if layout.is_dir() else []
    )
    with out.open('wb') as f:
        for path in files:
            data = path.read_bytes()
            relative = path.relative_to(cache)
            lines.append(f'{relative}\t{hashlib.sha256(data).hexdigest()}\t{len(data)}\t{offset}\n')
            if identity is None and relative.parts[1] == CONTENT:
                identity = data[8:36].hex()
            f.write(data)
            offset += len(data)
    return lines, identity


def main() -> None:
    """Make the bucket into its pack."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ('precompile', 'mock', 'nvk', 'root', 'store', 'qc', 'lines', 'out', 'index', 'report'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--embedded', type=Path, action='append', default=[])
    parser.add_argument('--seed', type=Path)
    parser.add_argument('--seed-pack', type=Path, action='append', default=[])
    parser.add_argument('--guess-dispatch', action='store_true')
    parser.add_argument('--unused-list', type=Path)
    parser.add_argument('--unused', action='append', default=[])
    args = parser.parse_args()
    text = args.lines.read_text().split(DERIVED + '\n')
    corpus, derived = by_kind(text[0].splitlines()), by_kind(text[1].splitlines() if len(text) > 1 else [])
    work = Path.cwd() / f'_bucket_{args.lines.stem}'
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    tools = {'precompile': args.precompile, 'libmock_iokit.dylib': args.mock, 'libvulkan_nouveau.dylib': args.nvk}
    for name, source in tools.items():
        place(source, work / name)
    store_view(work / 'store', args.root, json.loads(args.store.read_text()), args.embedded)
    began = time.monotonic()
    report = make(work, args, corpus, derived) if any(corpus.values()) or any(derived.values()) else {'runs': []}
    seconds = round(time.monotonic() - began, 1)
    report['lines'] = sum(map(len, corpus.values())) + sum(map(len, derived.values()))
    lines, report['identity'] = pack(work / 'cache', args.out)
    args.index.write_text(''.join(lines))
    args.report.write_text(json.dumps(report, indent=1) + '\n')
    if args.unused_list:
        args.unused_list.write_text(''.join(path + '\n' for path in args.unused))
    shutil.rmtree(work)
    crashed, refused = len(report.get('crashed', [])), len(report.get('refused', []))
    made = f'{report["lines"]} lines in {seconds} s'
    print(f'bucket {args.lines.stem}: {made}, {refused} refused, {crashed} crashed')


if __name__ == '__main__':
    main()
