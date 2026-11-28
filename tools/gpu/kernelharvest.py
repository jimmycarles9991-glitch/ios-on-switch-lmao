#!/usr/bin/env python3
"""Add the compute kernels a device dispatched at run time, read from pulled logarchives, to the precompile corpus.

usage: bazel run //tools/gpu:kernelharvest -- <boot.logarchive>... [--build <IPSW build>] [--corpus <corpus.log>]
                                               [--libraries <directory or tar>]... [--dry-run]
       bazel run //tools/gpu:kernelharvest -- --ndjson <file>...   (`log show --style ndjson --info` output instead)

The lines it reads, os_log's (the runtime's, TegraGM20BMetalComplete.mm and TegraGM20BMetalCompleteRemote.mm):
    SwitchMetal: kernel harvest <name> <i>/<n> <part>
        the n parts, 700 characters each, of "lib=<library> function=<function>[ spec] kernel | <compute key>": a kernel
        the shader compiler answered at run time (debug builds), its library a system library's path or data:<SHA-256
        of its bytes>, spec where its function was made with constant values
    SwitchMetal: facts harvest <name> <i>/<n> <part>
        the n parts, 700 characters each, of "lib=<library> function=<function>[ spec] facts | <compute state key>": a
        compute state created without a dispatch whose facts the shader compiler answered (emitted as tg=0,0,0 so the
        pack generator populates PackComputeFacts without a PackKernel)
    SwitchMetal: a kernel the pack lacks, made at its first dispatch of <x,y,z> threads in <us> us[ (<QoS>)]: <function>
    <key>
    SwitchMetal: a kernel the pack lacks <is refused|was not made> at <x,y,z> threads: <why> | <function> <key>
        a kernel made by an older runtime, and one refused: <key> is the compute key with its constants last
        (TegraGM20BMetalComplete.mm constantsLast: "<AIR> | max=.. tgm=.. | tg=.. | tgmem=.. | tex=.. | smp=..[ |
        region=1] | constants <constants>")
A compute key is compute_key.h computeKey's text. Each kernel becomes the line
    D ok=1 tg=<x,y,z> edge=0,0,0 threads=0 [tgmem=..] [tex=..] [smp=..] os=<build> | form=d opt=0 max=.. tgm=..
        label=- | k <function> lib=<library>[ spec] <constants>[ | in <stage input>] |
which pack/kernel_shapes.h reads back to the key: a texture item stating no address map (a key from before keys
stated them) takes its format's own (stateFormatMaps). The older lines name no library: a kernel's is the one of
--libraries (data libraries, each <SHA-256>.metallib, in a directory or a tar's data/) whose function of its name has
its AIR, else the one library the corpus's D lines and specializations (S lines) name for the function. A line the
corpus already holds for the build is not added again. Boots and builds are read as tools/gpu/logharvest.py reads them.

Left out and counted: a region dispatch (its edges are not in its key), a kernel that links functions, one binding a
texture kernel_shapes.h takes no shape of, a key the log cut, a compute.kernel route line (no key), and a kernel no
library is found for, named with its AIR and textures' formats so its library can be pulled.
"""

import argparse
import importlib.util
import re
import sys
import tarfile
from collections import Counter, defaultdict
from collections.abc import Iterable
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import logharvest


def library_module():
    """Load precompile/libraries.py, whose functions() reads a metallib's function names and AIR hashes."""
    path = Path(__file__).resolve().parents[2] / logharvest.PRECOMPILE / 'libraries.py'
    spec = importlib.util.spec_from_file_location('precompile_libraries', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


libraries = library_module()

AIR = r'(?P<air>[0-9a-f]{64})'
SHAPE = (
    r' \| max=(?P<max>\d+) tgm=(?P<tgm>[01]) \| tg=(?P<tg>\d+,\d+,\d+) \| tgmem=(?P<mem>\S*) \| tex=(?P<tex>\S*)'
    r' \| smp=(?P<smp>\S*)(?P<region> \| region=1)?'
)
# constantsLast puts " | constants" and the key's constants part, after one space, last
MADE = re.compile(
    r'SwitchMetal: a kernel the pack lacks, made at its first dispatch of \S+ threads in \d+ us(?: \([^)]*\))?: '
    r'(?P<function>\S+) ' + AIR + SHAPE + r' \| constants(?P<constants>.*)$',
    re.DOTALL,
)
REFUSED = re.compile(
    r'SwitchMetal: a kernel the pack lacks (?:is refused|was not made) at \S+ threads: .*? \| (?P<function>\S+) '
    + AIR
    + SHAPE
    + r' \| constants(?P<constants>.*)$',
    re.DOTALL,
)
KEY = re.compile(r'kernel \| ' + AIR + r' \| (?P<constants>.*?)' + SHAPE + r'$', re.DOTALL)
FACTS_KEY = re.compile(r'facts \| ' + AIR + r' \| (?P<constants>.*?) \| max=(?P<max>\d+) tgm=(?P<tgm>[01])$', re.DOTALL)
HEAD = re.compile(r'lib=(?P<lib>\S+) function=(?P<function>\S+)(?P<spec> spec)? ')
PART = re.compile(r'SwitchMetal: kernel harvest ([0-9a-f]{16}) (\d+)/(\d+) (.*)$', re.DOTALL)
FACTS_PART = re.compile(r'SwitchMetal: facts harvest ([0-9a-f]{16}) (\d+)/(\d+) (.*)$', re.DOTALL)
FACTS_ONLY_TG = '0,0,0'
ROUTE = 'compute.kernel: a kernel the pack lacks ('
PART_CHARS = 700
PREDICATE = ' OR '.join(
    f'eventMessage CONTAINS "{text}"'
    for text in (
        'a kernel the pack lacks',
        'SwitchMetal: kernel harvest ',
        'SwitchMetal: facts harvest ',
        'OS version: ',
        'deviceBuildVersion is',
        'iPhone OS ',
    )
)
LINKED = ('kl ', 'kp ', 'kb ', 'kg ')


@dataclass(frozen=True)
class Kernel:
    function: str
    air: str
    constants: str  # the key's constants part: its function's constants, then " | in ..." and linked functions
    max: str
    tgm: str
    tg: str
    mem: str
    tex: str
    smp: str
    region: bool
    lib: str = ''  # its library where its line names it
    spec: bool = False


@dataclass
class Boot:
    builds: set[str] = field(default_factory=set)
    kernels: dict[tuple[str, Kernel], None] = field(default_factory=dict)  # (process, kernel), in the order first seen
    parts: dict[tuple[str, str], dict[int, str]] = field(default_factory=lambda: defaultdict(dict))
    totals: dict[tuple[str, str], int] = field(default_factory=dict)
    facts_parts: dict[tuple[str, str], dict[int, str]] = field(default_factory=lambda: defaultdict(dict))
    facts_totals: dict[tuple[str, str], int] = field(default_factory=dict)
    cut: Counter = field(default_factory=Counter)
    unkeyed: Counter = field(default_factory=Counter)  # route lines (compute.kernel) that name the kernel's shape alone


def kernel_of(m: re.Match, function: str, constants: str, lib: str = '', spec: bool = False) -> Kernel:
    return Kernel(
        function,
        m['air'],
        constants,
        m['max'],
        m['tgm'],
        m['tg'],
        m['mem'],
        m['tex'],
        m['smp'],
        bool(m['region']),
        lib,
        spec,
    )


def read_boots(stream: Iterable[dict]) -> dict[str, Boot]:
    boots: dict[str, Boot] = defaultdict(Boot)
    for record in stream:
        boot = boots[record.get('bootUUID', '-')]
        message = record.get('eventMessage') or ''
        process = (record.get('processImagePath') or '-').rsplit('/', 1)[-1]
        for pattern in logharvest.BUILD_LINES:
            if m := pattern.search(message):
                boot.builds.add(m[1])
        if m := PART.search(message):
            boot.parts[process, m[1]][int(m[2])] = m[4]
            boot.totals[process, m[1]] = int(m[3])
        elif m := FACTS_PART.search(message):
            boot.facts_parts[process, m[1]][int(m[2])] = m[4]
            boot.facts_totals[process, m[1]] = int(m[3])
        elif m := MADE.search(message) or REFUSED.search(message):
            boot.kernels.setdefault((process, kernel_of(m, m['function'], m['constants'].removeprefix(' '))))
        elif ROUTE in message:
            boot.unkeyed[process] += 1
        elif 'a kernel the pack lacks' in message:
            boot.cut[process] += 1
    for boot in boots.values():
        for (process, name), parts in boot.parts.items():
            total = boot.totals[process, name]
            if sorted(parts) != list(range(1, total + 1)):
                boot.cut[process] += 1
                continue
            text = ''.join(parts[i].ljust(PART_CHARS) for i in range(1, total)) + parts[total]
            head, key = HEAD.match(text), None
            if head:
                key = KEY.match(text[head.end() :])
            if not key:
                boot.cut[process] += 1
                continue
            kernel = kernel_of(key, head['function'], key['constants'], head['lib'], bool(head['spec']))
            boot.kernels.setdefault((process, kernel))
        for (process, name), parts in boot.facts_parts.items():
            total = boot.facts_totals[process, name]
            if sorted(parts) != list(range(1, total + 1)):
                boot.cut[process] += 1
                continue
            text = ''.join(parts[i].ljust(PART_CHARS) for i in range(1, total)) + parts[total]
            head, key = HEAD.match(text), None
            if head:
                key = FACTS_KEY.match(text[head.end() :])
            if not key:
                boot.cut[process] += 1
                continue
            kernel = Kernel(
                head['function'],
                key['air'],
                key['constants'],
                key['max'],
                key['tgm'],
                FACTS_ONLY_TG,
                '',
                '',
                '',
                False,
                head['lib'],
                bool(head['spec']),
            )
            boot.kernels.setdefault((process, kernel))
    return boots


class Libraries:
    """The library a kernel's function is made from: a data library holding it with its AIR (--libraries), else the one
    library the corpus's D lines and specializations (S lines) name for its function name.
    """

    def __init__(self, files: list[tuple[str, bytes]], corpus: list[str], build: str | None) -> None:
        self.by_air: dict[tuple[str, str], set[str]] = defaultdict(set)
        self.airs: dict[tuple[str, str], set[str]] = defaultdict(set)  # a data library's AIR of a function
        for name, data in files:
            for function, air in (libraries.functions(data, build) or {}).items():
                self.by_air[function, air].add(f'data:{name}')
                self.airs[f'data:{name}', function].add(air)
        self.by_name: dict[str, set[str]] = defaultdict(set)
        for line in corpus:
            if m := re.search(r' \| k (\S+) lib=(\S+)', line) if line.startswith('D ') else None:
                self.by_name[m[1]].add(m[2])
            elif m := logharvest.FUNCTION_LIBRARY[1].match(line):
                self.by_name[m[2]].add(m[1])

    def of(self, kernel: Kernel) -> tuple[str, str]:
        """Give the kernel's library, or '' and why there is none."""
        if kernel.lib and kernel.lib != 'data':
            return kernel.lib, ''
        held = self.by_air.get((kernel.function, kernel.air))
        if held:
            return min(held), ''
        named = {lib for lib in self.by_name.get(kernel.function, set()) if not self.airs.get((lib, kernel.function))}
        if len(named) == 1:
            return named.pop(), ''
        formats = sorted({t.split(':')[1] for t in kernel.tex.split(',') if t})
        return '', f'no library holds {kernel.function} {kernel.air} (textures of {",".join(formats) or "none"})'


def library_files(sources: list[Path]) -> Iterable[tuple[str, bytes]]:
    """Each data library of `sources`, by the SHA-256 its file is named by: a directory's *.metallib, a tar's data/."""
    for source in sources:
        if source.is_dir():
            for path in sorted(source.glob('*.metallib')):
                yield path.stem, path.read_bytes()
        elif tarfile.is_tarfile(source):
            with tarfile.open(source) as tar:
                for member in tar:
                    path = Path(member.name)
                    if member.isfile() and path.parent.name == 'data' and path.suffix == '.metallib':
                        yield path.stem, tar.extractfile(member).read()


def corpus_line(kernel: Kernel, found: Libraries, build: str) -> tuple[str | None, str]:
    """Give the D line of `kernel`, or None and why there is none."""
    if kernel.region:
        return None, 'a region dispatch (its edges are not in its key)'
    constants, *segments = kernel.constants.split(' | ')
    if any(s.startswith(LINKED) for s in segments):
        return None, 'a kernel that links functions'
    if any(not s.startswith('in ') for s in segments):
        return None, 'an unknown segment of its constants'
    if any(t.split(':')[2:5] != ['2', '1', '1'] for t in kernel.tex.split(',') if t):
        return None, 'a texture of another type than a one-level 2D one (kernel_shapes.h parseTextures)'
    library, why = found.of(kernel)
    if not library:
        return None, why
    fields = (('tgmem', kernel.mem), ('tex', kernel.tex), ('smp', kernel.smp))
    head = f'D ok=1 tg={kernel.tg} edge=0,0,0 threads=0' + ''.join(f' {n}={v}' for n, v in fields if v)
    function = f'k {kernel.function} lib={library}{" spec" if kernel.spec else ""} {constants}'
    stage_in = ''.join(f' | {s}' for s in segments)
    state = f'form=d opt=0 max={kernel.max} tgm={kernel.tgm} label=-'
    return f'{head} os={build} | {state} | {function}{stage_in} | ', ''


def shape(line: str) -> tuple | None:
    """Give what a D line keys its kernel by but its AIR and the builds it names, None for another line."""
    if not line.startswith('D '):
        return None
    head, _, rest = line.partition(' | ')
    fields = dict(f.split('=', 1) for f in head.split() if '=' in f)
    state, _, stages = rest.partition(' | k ')
    items = dict(f.split('=', 1) for f in state.split() if '=' in f)
    if items.get('form') == 'f':
        items['max'], items['tgm'] = '0', '0'
    return (
        fields.get('tg'),
        fields.get('tgmem', ''),
        fields.get('tex', ''),
        fields.get('smp', ''),
        items.get('max', '0'),
        items.get('tgm', '0'),
        ' '.join(stages.rstrip(' |').split()),
    )


def in_corpus(corpus: list[str], build: str) -> set[tuple]:
    """Give the shapes of the corpus's D lines that name `build`."""
    return {shape(line) for line in corpus if line.startswith('D ') and build in logharvest.os_tags(line)}


@dataclass
class Report:
    boots: list[str] = field(default_factory=list)
    added: Counter = field(default_factory=Counter)
    present: Counter = field(default_factory=Counter)
    left: Counter = field(default_factory=Counter)
    formats: Counter = field(default_factory=Counter)  # the texture formats the added kernels bind


def harvest(
    boots: dict[str, Boot], corpus: list[str], forced: str | None, sources: list[Path]
) -> tuple[list[str], Report]:
    report, new, files = Report(), [], list(library_files(sources))
    for uuid, boot in sorted(boots.items()):
        build = logharvest.boot_build(uuid, boot, forced)
        if not build:
            report.boots.append(f'{uuid}: names no OS build, skipped (--build names it)')
            continue
        report.boots.append(f'{uuid}: {build}')
        for process, count in boot.cut.items():
            report.left[process, 'a kernel line the log cut'] += count
        for process, count in boot.unkeyed.items():
            report.left[process, 'a compute.kernel route line, which names no key'] += count
        found = Libraries(files, corpus + new, build)
        held = in_corpus(corpus + new, build)
        held_states = {s[4:] for s in held if s}
        for process, kernel in boot.kernels:
            line, why = corpus_line(kernel, found, build)
            if not line:
                report.left[process, why] += 1
                continue
            key = shape(line)
            if key in held or (kernel.tg == FACTS_ONLY_TG and key[4:] in held_states):
                report.present[process] += 1
                continue
            held.add(key)
            held_states.add(key[4:])
            new.append(line)
            report.added[process] += 1
            report.formats.update(t.split(':')[1] for t in kernel.tex.split(',') if t)
    return new, report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('archives', nargs='+')
    parser.add_argument('--ndjson', action='store_true', help='the inputs are `log show --style ndjson` output')
    parser.add_argument('--build', help='the OS build of every boot (each boot must name it or none)')
    parser.add_argument('--corpus', type=Path, default=logharvest.PROJECT / logharvest.PRECOMPILE / 'corpus.log')
    parser.add_argument('--libraries', type=Path, action='append', default=[], help='data libraries: a dir or a tar')
    parser.add_argument('--dry-run', action='store_true', help='print the lines instead of adding them')
    args = parser.parse_args(argv)
    corpus = args.corpus.read_text().splitlines()
    boots = read_boots(logharvest.records(args.archives, args.ndjson, PREDICATE))
    new, report = harvest(boots, corpus, args.build, args.libraries)
    for line in report.boots:
        print('boot', line)
    for process in sorted(set(report.added) | set(report.present)):
        print(f'{process}: {report.added[process]} added, {report.present[process]} already in the corpus')
    for (process, why), count in sorted(report.left.items()):
        print(f'{process}: {count} left out: {why}')
    print('formats the added kernels bind:', ' '.join(f'{f}x{n}' for f, n in sorted(report.formats.items())) or '-')
    if args.dry_run:
        print('\n'.join(new))
    elif new:
        with args.corpus.open('a') as f:
            f.write(''.join(line + '\n' for line in new))
    print(f'{args.corpus}: {len(new)} lines {"would be " if args.dry_run else ""}added')
    return 0


if __name__ == '__main__':
    sys.exit(main())
