#!/usr/bin/env python3
"""The Metal libraries the shader cache precompile (precompile.mm) loads on the host.

The seed build (tools/gpu/seed.py) lays them out as the store precompile.mm reads:

  <store>/root/<device path>        a library the device loaded from a file (the journal's lib=<path>): the IPSW's
                                    volumes', crawled by //gpu/seed:crawl (tools/gpu/shaders/crawl.py)
  <store>/data/<sha256>.metallib    a library the device made from bytes (lib=data:<sha256>): any of the above or of the
                                    frontend's own (TwoPlaneStandIn.h) whose bytes they are, or one only the device made
                                    (Core Image's compiled kernels: @gpu_device_data, precompile/harvest.py --pack)
  <names.json>                      every function name of the pinned libraries -> their device paths (`names`, a build
                                    action)

usage: libraries.py pins <mounted OS volume>
           the `shasum -a 256` listing of every Metal library of the volume with a function (gpu/apple-libs.sha256)
       libraries.py names --build <IPSW build> <root> <names.json> [<device path>...]
           names.json of the libraries at <root>/<device path> (none given: every file below <root>), each by the
           AIR slice that build's Metal loads of a universal file
       libraries.py resolve <names.json> <corpus.log> <out.log>
           the corpus with lib=<path> added to the stages of journal lines from before the journal named libraries
           (no lib=) whose function names are in exactly one library (QuartzCore's names stay QuartzCore's, the
           precompile's default; a line whose names are in two or three is replayed in each), and the coverage table:
           per library, lines named and resolved

A journal line names its library from the frontend that writes lib= on; an older line's library is only guessed by a
unique function name, and the table keeps the two apart. A wrong guess costs nothing on the device: m2v's keys hash the
function's AIR, so a translation of another library's function is never found there.
"""

import hashlib
import json
import re
import struct
import sys
from collections import Counter, defaultdict
from collections.abc import Callable
from pathlib import Path
from urllib.parse import quote, unquote

QC = '/System/Library/Frameworks/QuartzCore.framework/default.metallib'


AIR64 = 0x01000017
# drivers/NativeMetal/MetallibSlice.h's kAirSliceRules (libraries_test holds the two the same): per IPSW build, whether
# its Metal loads the first AIR64 slice, else its AIR ceiling, whether the device is below featureProfile 8 and the
# subtype of AIR 2.9
AIR_SLICE_RULES = {
    '21H461': (True, 0, False, 0),
    '24B119': (False, 0x20009, True, 0xD),
}


def _picks(rule: tuple[bool, int, bool, int], current: int | None, candidate: int) -> bool:
    _, ceiling, below_profile8, air29 = rule
    if candidate > ceiling:
        return False
    if not below_profile8:
        return current is None or candidate > current
    if current is None:
        return candidate <= air29
    return candidate < air29 and (current >= air29 or candidate > current)


def air_slice(data: bytes, build: str | None) -> bytes:
    """Return the AIR slice of a universal metallib `build`'s Metal loads (MetallibSlice.h), `data` itself if thin.

    No build: the first MTLB slice. b'' when none is taken.
    """
    if data[:4] != b'\xca\xfe\xba\xbe':
        return data
    rule = AIR_SLICE_RULES[build] if build else None
    (count,) = struct.unpack_from('>I', data, 4)
    picked, subtype = None, None
    for i in range(count):
        cpu, sub, offset, size, _ = struct.unpack_from('>IIIII', data, 8 + 20 * i)
        if rule is None:
            if data[offset : offset + 4] == b'MTLB':
                return data[offset : offset + size]
        elif cpu != AIR64:
            continue
        elif rule[0]:
            return data[offset : offset + size]
        elif _picks(rule, subtype, sub):
            picked, subtype = (offset, size), sub
    return data[picked[0] : picked[0] + picked[1]] if picked else b''


def names(data: bytes, build: str | None = None) -> list[str] | None:
    """Return the function names of a metallib's bytes (air_slice of a fat one), or None if not a library."""
    data = air_slice(data, build)
    if data[:4] != b'MTLB' or len(data) < 0x58:
        return None
    offset, _ = struct.unpack_from('<QQ', data, 24)  # the function list: u32 count, then entries of u32 size and tags
    if offset + 4 > len(data):
        return None
    (count,) = struct.unpack_from('<I', data, offset)
    at, found = offset + 4, []
    for _ in range(count):
        if at + 4 > len(data):
            return None
        (size,) = struct.unpack_from('<I', data, at)
        tag, end = at + 4, at + size
        while tag + 6 <= end and data[tag : tag + 4] != b'ENDT':
            (length,) = struct.unpack_from('<H', data, tag + 4)
            if data[tag : tag + 4] == b'NAME':
                found.append(data[tag + 6 : tag + 6 + length].split(b'\0')[0].decode('utf-8', 'replace'))
            tag += 6 + length
        at = end
    return found


def pins(volume: str) -> None:
    """Print `<sha256>  <path>` of every *.metallib below `volume` that is a Metal library with a function.

    By path (LC_ALL=C).
    """
    for path in sorted(str(p.relative_to(volume)) for p in Path(volume).rglob('*.metallib') if p.is_file()):
        data = (Path(volume) / path).read_bytes()
        if names(data):
            print(f'{hashlib.sha256(data).hexdigest()}  {path}')


def index(build: str, root: str, out: str, devices: list[str]) -> None:
    """Write names.json of the libraries at <root>/<device path>, as IPSW `build`'s Metal loads them.

    Every function name -> the device paths of those that have it, each list in the order of the paths' components.
    """
    table = defaultdict(list)
    for device in sorted(('/' + d.lstrip('/') for d in devices), key=lambda d: Path(d).parts):
        found = names((Path(root) / device.lstrip('/')).read_bytes(), build)
        if found is None:
            sys.exit(f'{root}/{device} is not a Metal library')
        for name in set(found):
            table[name].append(device)
    Path(out).write_text(json.dumps(dict(sorted(table.items())), indent=0) + '\n')
    libraries = len({p for v in table.values() for p in v})
    print(f'{out}: {len(table)} function names in {libraries} libraries')


STAGE = re.compile(r'\| (vl|vp|fl|fp|[vfklp]) (\S+)( [^|]*?)?(?= \|)')
INSTALLED_APP = re.compile(
    r'^/private/var/containers/Bundle/Application/'
    r'[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}/([^/]+\.app/.+)$'
)


def library_path(library: str, available: set[str]) -> str:
    path = unquote(library)
    if path.startswith('/System/Cryptexes/OS/'):
        path = path[len('/System/Cryptexes/OS') :]
    if path.startswith('/var/'):
        path = '/private' + path
    if path in available:
        return quote(path, safe='/:')
    installed = INSTALLED_APP.fullmatch(path)
    if installed:
        staged = '/private/var/staged_system_apps/' + installed[1]
        if staged in available:
            return quote(staged, safe='/:')
    return library


def relocate(line: str, available: set[str]) -> str:
    return re.sub(r'(?<= )lib=(\S+)', lambda m: 'lib=' + library_path(m[1], available), line)


GUESSES = (
    3  # a name in up to this many libraries is replayed in each of them (seed space only: m2v's keys hash the AIR)
)

Stage = tuple[re.Match[str], str | None, list[str | None] | None]


def _stages(line: str, kind: str, candidates: Callable[[str], list[str | None]]) -> list[Stage]:
    # Each stage: (its text, the journal's library, or the candidates of its name). An S line is one stage.
    stages = []
    if kind == 'S':
        m = re.match(r'S ok=1 (lib=(\S+) )?name=(\S+) ', line)
        if m:
            stages.append((m, m[2], None if m[2] else candidates(m[3])))
    else:
        for m in STAGE.finditer(line):
            lib = next((t[4:] for t in (m[3] or '').split() if t.startswith('lib=')), None)
            if m[2] != '-':
                stages.append((m, lib, None if lib else candidates(m[2])))
    return stages


def _choices(stages: list[Stage]) -> tuple[str, list[str | None]]:
    open_ = [c for _, lib, c in stages if not lib]
    if not open_:
        return ('journaled' if stages else 'unknown'), [None]
    # The stages' functions are taken to come from one library: the libraries every open stage's name is in.
    common = set(open_[0]).intersection(*open_[1:])
    how = (
        'qc'
        if common == {None}
        else 'resolved'
        if len(common) == 1
        else 'guessed'
        if 1 < len(common) <= GUESSES
        else 'ambiguous'
        if any(open_)
        else 'unknown'
    )
    return how, sorted(common, key=lambda c: c or '') if how in ('qc', 'resolved', 'guessed') else [None]


def _with_library(line: str, kind: str, stages: list[Stage], choice: str | None) -> str:
    text = line
    for m, lib, _ in reversed(stages):  # from the end, so earlier offsets hold
        if lib or not choice:
            continue
        if kind == 'S':
            text = text[: m.start(3) - 5] + f'lib={choice} ' + text[m.start(3) - 5 :]
        else:
            text = text[: m.end(2)] + ' lib=' + choice + text[m.end(2) :]
    return text


def resolve(index_json: str, corpus: str, out: str) -> None:
    table = json.loads(Path(index_json).read_text())
    available = {path for paths in table.values() for path in paths}
    qc = {n for n, paths in table.items() if QC in paths}
    lines, rows = [], defaultdict(Counter)

    def candidates(name: str) -> list[str | None]:
        """Return the libraries a function from before the journal named libraries may be from.

        QuartzCore's (None, the default) when it has the name, else every library holding it.
        """
        return [None] if name in qc else table.get(name, [])

    for original in Path(corpus).read_text().splitlines():
        kind = original[:1]
        if kind not in 'SRCD':
            lines.append(original)
            continue
        line = relocate(original, available)
        stages = _stages(line, kind, candidates)
        how, choices = _choices(stages)
        for choice in choices:
            lines.append(_with_library(line, kind, stages, choice))
            first = next((lib for _, lib, _ in stages if lib), None) or choice or QC
            rows[first if how in ('qc', 'journaled', 'resolved', 'guessed') else how][(kind, how)] += 1
    Path(out).write_text('\n'.join(lines) + '\n')
    header = (
        'library (lines: qc = QuartzCore by default, journaled, resolved by a unique name, guessed: in each '
        f'of up to {GUESSES})'
    )
    print(f'{header:<80} ')
    for lib, counts in sorted(rows.items(), key=lambda kv: -sum(kv[1].values())):
        tally = ' '.join(f'{k}.{h}={n}' for (k, h), n in sorted(counts.items()))
        print(f'  {lib:<78} {tally}')


def main() -> None:
    verb, rest = (sys.argv[1], sys.argv[2:]) if len(sys.argv) > 1 else (None, [])
    if verb == 'pins' and len(rest) == 1:
        pins(rest[0])
    elif verb == 'names' and len(rest) >= 4 and rest[0] == '--build' and rest[1] in AIR_SLICE_RULES:
        build, root, out, devices = rest[1], rest[2], rest[3], rest[4:]
        index(build, root, out, devices or [str(p.relative_to(root)) for p in Path(root).rglob('*') if p.is_file()])
    elif verb == 'resolve' and len(rest) == 3:
        resolve(*rest)
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
