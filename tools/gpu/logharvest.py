#!/usr/bin/env python3
"""Add the render states a device made at run time, read from a pulled logarchive, to the precompile corpus.

usage: bazel run //tools/gpu:logharvest -- <boot.logarchive>... [--build <IPSW build>] [--corpus <corpus.log>]
                                            [--dry-run]
       bazel run //tools/gpu:logharvest -- --ndjson <file>...   (`log show --style ndjson --info` output instead)

The lines it reads, os_log's (the raw runtime's in debug builds, RawComplete.mm and RawMetalCubic.mm):
    raw: a state the pack lacks <made|not made|refused> in <ms> ms (<QoS>) after <ms> ms for a compile slot: <name>
        <key>
        (one line) <name>: the runtime's 16-hex-digit cache name of the state; <key>: its first 400 characters
    raw: harvest <name> <i>/<n> <part>
        the n parts, 700 characters each, of "label=<descriptor label, spaces as _> <whole key>"
    <process>[<pid>] state.variant_unpacked: ...refused: | cubic <binds> of <state> <key>
        a packed state's cubic variant the pack lacks; <state>: Pack::fnv of the state's key, 16 hex digits
    OS version: <build>, deviceBuildVersion is "<build>", iPhone OS <version> (<build>)
A key is RawPipelineKey.h pipelineKey's text: "<descriptor fields>  | v <function> <constants> | f <function>
<constants>[ | <vl|vp|fl|fp> <linked function>...][ | l <framework>]", then a variant's name (RawMetalCubic.mm
renderVariant): "[ | cubic <binds>][ | tests=<n>][ | xr2p][ | xrs=<slots>][ | wtex=<slot>:<format>,...]". Each state
becomes the line
    R ok=1 label=<Core Animation label, else -> os=<build> <fields> | v <function> lib=<path> <constants> | f ... |
        [cubic <binds> | ][xrs=<slots> | ][wtex=<slot>:<format>,... | ]
built so that the pack generator's rawpack.state_key gives back the logged key exactly (QuartzCore's stages name no
lib=); a line the corpus already holds for that build (untagged, or tagged with it) is not added again. A variant's
state gets its own line first.

A USB pull holds several boots with overlapping clocks: each boot (log show's bootUUID) is read alone, and names its OS
build in its own lines ("OS version: 24B119"). A boot naming none is skipped unless --build names it; one naming
another build than --build is refused. A key's test class (" | tests=<n>") and two-plane pass (" | xr2p") are left
off, as the pack builder makes those of every line. Left out and counted: our own apps' states (SwitchHWTest*), a key
the log cut with no whole parts (or parts that disagree with it), a key whose library the log does not name (" | air
...": a bundle's), one linking functions, a Core Animation state with no label, a framework whose library path the
build's archive inventory and the corpus do not name, a variant whose state the corpus lacks, and the states the device
refused.
"""

import argparse
import importlib.util
import json
import os
import re
import subprocess
import sys
import types
from collections import Counter, defaultdict
from collections.abc import Iterable, Iterator
from dataclasses import dataclass, field
from pathlib import Path


def generator_module() -> types.ModuleType:
    """Load the pack generator (raw/pack/rawpack.py), whose state_key a harvested line must give back its key by.

    By path: tools/gpu/rawpack.py, beside this file, has the same module name.
    """
    path = Path(__file__).resolve().parents[2] / 'drivers/TegraGM20BMetal/raw/pack/rawpack.py'
    spec = importlib.util.spec_from_file_location('rawpack_generator', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


rawpack = generator_module()

PROJECT = Path(os.environ.get('BUILD_WORKSPACE_DIRECTORY') or Path(__file__).resolve().parents[2])
PRECOMPILE = 'drivers/TegraGM20BMetal/precompile'
MADE = re.compile(
    r'raw: a state the pack lacks (made|not made|refused) in .*? for a compile slot: ([0-9a-f]{16}) (.*)$', re.DOTALL
)
CUT = 400  # RawComplete.mm's made line: %.400s
PART = re.compile(r'raw: harvest ([0-9a-f]{16}) (\d+)/(\d+) (.*)$', re.DOTALL)
VARIANT = re.compile(r'state\.variant_unpacked: .*?refused:( \| .*?) of ([0-9a-f]{16}) ')
VARIANT_ANY = 'state.variant_unpacked: '
BUILD = r'([0-9]{2}[A-Z][0-9]{1,4}[a-z]?)'
BUILD_LINES = (
    re.compile(r'\bOS version: ' + BUILD + r'\b'),
    re.compile(r'deviceBuildVersion is "' + BUILD + '"'),
    re.compile(r'\biPhone OS [0-9.]+ \(' + BUILD + r'\)'),
)
# A variant's name segments a corpus line carries (rawpack.state_key: cubic binds, sampled two-plane slots, written
# formats), and those the pack builder makes of every line it takes (directpack.mm makeState)
LINE_VARIANTS = ('cubic ', 'xrs=', 'wtex=')
BUILDER_FORMS = re.compile(r' \| (tests=\d+|xr2p)(?= \| |$)')
OWN_APPS = ('SwitchHWTest',)
CA_LABEL = 'com.apple.coreanimation.'
FRAMEWORK_PATH = re.compile(r' lib=(/System/Library/(?:Private)?Frameworks/(\w+)\.framework/default\.metallib)')
PREDICATE = ' OR '.join(
    f'eventMessage CONTAINS "{text}"'
    for text in (
        'a state the pack lacks',
        'raw: harvest ',
        VARIANT_ANY,
        'OS version: ',
        'deviceBuildVersion is',
        'iPhone OS ',
    )
)


@dataclass
class Boot:
    builds: set[str] = field(default_factory=set)
    made: dict[tuple[str, str], tuple[str, str]] = field(default_factory=dict)  # (process, hash) -> (outcome, key)
    parts: dict[tuple[str, str], dict[int, str]] = field(default_factory=lambda: defaultdict(dict))
    totals: dict[tuple[str, str], int] = field(default_factory=dict)
    variants: set[tuple[str, str, str]] = field(default_factory=set)  # (process, variant name, state hash)
    unnamed: Counter = field(default_factory=Counter)  # variant lines that name no state hash, by process


def records(paths: list[str], ndjson: bool) -> Iterator[dict]:
    for path in paths:
        if ndjson:
            with open(path, errors='replace') as f:
                yield from events(f)
            continue
        cmd = ['/usr/bin/log', 'show', '--archive', path, '--info', '--style', 'ndjson', '--predicate', PREDICATE]
        with subprocess.Popen(cmd, stdout=subprocess.PIPE, text=True, errors='replace') as proc:
            yield from events(proc.stdout)
        if proc.returncode:
            sys.exit(f'{path}: log show failed ({proc.returncode})')


def events(lines: Iterable[str]) -> Iterator[dict]:
    for line in lines:
        if line.startswith('{'):
            record = json.loads(line)
            if record.get('eventType') == 'logEvent':
                yield record


def read_boots(stream: Iterable[dict]) -> dict[str, Boot]:
    boots: dict[str, Boot] = defaultdict(Boot)
    for record in stream:
        boot = boots[record.get('bootUUID', '-')]
        message = record.get('eventMessage') or ''
        process = (record.get('processImagePath') or '-').rsplit('/', 1)[-1]
        for pattern in BUILD_LINES:
            if m := pattern.search(message):
                boot.builds.add(m[1])
        if m := MADE.search(message):
            boot.made.setdefault((process, m[2]), (m[1], m[3]))
        elif m := PART.search(message):
            boot.parts[process, m[1]][int(m[2])] = m[4]
            boot.totals[process, m[1]] = int(m[3])
        elif m := VARIANT.search(message):
            boot.variants.add((process, m[1], m[2]))
        elif VARIANT_ANY in message:
            boot.unnamed[process] += 1
    return boots


def whole(boot: Boot, process: str, name: str) -> tuple[str, str] | None:
    """Give the label and whole key a state's harvest parts carry, None where a part is missing."""
    parts, total = boot.parts.get((process, name)), boot.totals.get((process, name))
    if not parts or sorted(parts) != list(range(1, total + 1)):
        return None
    text = ''.join(parts[i] for i in range(1, total + 1))
    label, _, key = text.partition(' ')
    return label.removeprefix('label='), key


class Libraries:
    """A framework's default.metallib path for one build: its archive inventory's, else the corpus's."""

    def __init__(self, build: str, corpus: list[str], inventory: Path) -> None:
        self.paths: dict[str, str] = {}
        rows = inventory.read_text().splitlines() if inventory.is_file() else []
        tagged = [line for line in corpus if build in os_tags(line)]
        for line in [*rows, *tagged, *corpus]:
            for path, name in FRAMEWORK_PATH.findall(line):
                self.paths.setdefault(name, path)


def corpus_line(key: str, label: str, build: str, libraries: Libraries) -> tuple[str | None, str]:
    """Give the R line whose state key is `key`, or None and why not."""
    segments = key.split(' | ')
    if len(segments) < 3 or not segments[1].startswith('v ') or not segments[2].startswith('f '):
        return None, 'not a render key'
    rest = segments[3:]
    framework = rest.pop(0)[2:].strip() if rest and rest[0].startswith('l ') else ''
    why = unharvestable(rest, framework, label, libraries)
    if why:
        return None, why
    lib = f' lib={libraries.paths[framework]}' if framework else ''
    stages = []
    for stage in segments[1:3]:
        kind, function, *constants = stage.split(' ', 2)
        stages.append(f'{kind} {function}{lib} {constants[0] if constants else ""}')
    head = segments[0].rstrip()
    tail = ''.join(f' | {s}' for s in rest)
    shown = label if label.startswith(CA_LABEL) else '-'
    line = f'R ok=1 label={shown} os={build} {head} | {" | ".join(stages)}{tail} | '
    if rawpack.state_key(line) != key:
        return None, 'its line does not give back its key'
    return line, ''


def unharvestable(rest: list[str], framework: str, label: str, libraries: Libraries) -> str:
    """Why a key's segments after its stages (and its framework) cannot make a corpus line, '' where they can."""
    if any(s.startswith('air ') for s in rest):
        return 'a bundle library (air): the log names no path'
    if any(s.startswith(rawpack.LINKED) for s in rest):
        return 'links functions'
    if any(not s.startswith(LINE_VARIANTS) for s in rest):
        return 'an unknown segment'
    if not framework and not label.startswith(CA_LABEL):
        return 'a QuartzCore state without its Core Animation label'
    if framework and framework not in libraries.paths:
        return f'no library path for {framework}'
    return ''


def in_corpus(corpus: list[str], build: str) -> set[str]:
    """Give the state keys the corpus holds for `build`: those of its lines that name it."""
    return {rawpack.state_key(line) for line in corpus if line.startswith('R ') and build in os_tags(line)}


def os_tags(line: str) -> list[str]:
    """Give the OS builds a line names (os=<build>[,<build>...] in its head)."""
    m = re.search(r' os=(\S+)', line.split(' | ', 1)[0])
    return m[1].split(',') if m else []


@dataclass
class Report:
    boots: list[str] = field(default_factory=list)
    added: Counter = field(default_factory=Counter)
    present: Counter = field(default_factory=Counter)
    left: Counter = field(default_factory=Counter)


def harvest(
    boots: dict[str, Boot], corpus: list[str], forced: str | None, precompile: Path
) -> tuple[list[str], Report]:
    report, new = Report(), []
    for uuid, boot in sorted(boots.items()):
        build = boot_build(uuid, boot, forced)
        if not build:
            report.boots.append(f'{uuid}: names no OS build, skipped (--build names it)')
            continue
        report.boots.append(f'{uuid}: {build}')
        for process, count in boot.unnamed.items():
            report.left[process, "a variant line that names no state hash (the runtime's older line)"] += count
        libraries = Libraries(build, corpus, precompile / 'archives' / f'{build}.tsv')
        new += harvest_boot(boot, build, libraries, in_corpus(corpus + new, build), report)
    return new, report


def boot_build(uuid: str, boot: Boot, forced: str | None) -> str | None:
    if forced and boot.builds and boot.builds != {forced}:
        sys.exit(f'boot {uuid} names {", ".join(sorted(boot.builds))}, not --build {forced}')
    if len(boot.builds) > 1:
        sys.exit(f'boot {uuid} names more than one build: {", ".join(sorted(boot.builds))}')
    return forced or next(iter(boot.builds), None)


def harvest_boot(boot: Boot, build: str, libraries: Libraries, held: set[str], report: Report) -> list[str]:
    """Give the lines of one boot's states `held` lacks (a variant's state first), adding their keys to it."""
    new = []
    for process, outcome, label, key, missing in states(boot, held):
        if process.startswith(OWN_APPS):
            report.left[process, 'our own app'] += 1
        elif outcome == 'refused':
            report.left[process, 'refused on the device'] += 1
        elif key is None:
            report.left[process, missing] += 1
        else:
            for wanted in dict.fromkeys((base_key(key), key)):
                if wanted in held:
                    report.present[process] += wanted == key
                    continue
                line, why = corpus_line(wanted, label, build, libraries)
                if not line:
                    report.left[process, why] += 1
                    break
                held.add(wanted)
                new.append(line)
                report.added[process] += 1
    return new


def states(boot: Boot, held: set[str]) -> Iterator[tuple[str, str, str, str | None, str]]:
    """Each state the boot made or refused once: its process, outcome, label, whole key, and why it has none.

    A key's test class and two-plane pass are left off: the pack builder makes them of each line itself.
    """
    for (process, name), (outcome, key) in sorted(boot.made.items()):
        parts = whole(boot, process, name)
        if parts and not (parts[1].startswith(key) if len(key) >= CUT else parts[1] == key):
            yield process, outcome, '-', None, 'its harvest parts disagree with its made line'
            continue
        label, full = parts or ('-', key if len(key) < CUT else None)
        yield process, outcome, label, untested(full), f'key cut at {CUT} characters, no whole parts'
    by_hash = {f'{rawpack.packfile.fnv(k.encode()):016x}': k for k in held}
    for process, variant, state in sorted(boot.variants):
        base = by_hash.get(state)
        yield process, 'made', '-', untested(base + variant) if base else None, 'its state is not in the corpus'


def untested(key: str | None) -> str | None:
    """Give a key without the forms the pack builder makes of each line it takes: its test class and two-plane pass."""
    if not key:
        return key
    name = rawpack.packfile.variant_name(key)
    return key[: len(key) - len(name)] + BUILDER_FORMS.sub('', name)


def base_key(key: str) -> str:
    """Give the key of the state a variant's key names (runtime/raw_pack.h variantNameAt)."""
    return key[: len(key) - len(rawpack.packfile.variant_name(key))]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('archives', nargs='+')
    parser.add_argument('--ndjson', action='store_true', help='the inputs are `log show --style ndjson` output')
    parser.add_argument('--build', help='the OS build of every boot (each boot must name it or none)')
    parser.add_argument('--corpus', type=Path, default=PROJECT / PRECOMPILE / 'corpus.log')
    parser.add_argument('--dry-run', action='store_true', help='print the lines instead of adding them')
    args = parser.parse_args(argv)
    corpus = args.corpus.read_text().splitlines()
    new, report = harvest(read_boots(records(args.archives, args.ndjson)), corpus, args.build, args.corpus.parent)
    for line in report.boots:
        print('boot', line)
    for process in sorted(set(report.added) | set(report.present)):
        print(f'{process}: {report.added[process]} added, {report.present[process]} already in the corpus')
    for (process, why), count in sorted(report.left.items()):
        print(f'{process}: {count} left out: {why}')
    if args.dry_run:
        print('\n'.join(new))
    elif new:
        with args.corpus.open('a') as f:
            f.write(''.join(line + '\n' for line in new))
    print(f'{args.corpus}: {len(new)} lines {"would be " if args.dry_run else ""}added')
    return 0


if __name__ == '__main__':
    sys.exit(main())
