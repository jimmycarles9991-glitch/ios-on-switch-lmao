"""How much of what Apple declares (declared.py's inventory) the raw pack holds and the device was seen making.

usage: coverage.py --inventory <declared.tsv> --pack <the pack's states> [--harvest <corpus.log>...] [--missing <out>]

--pack: the R lines the pack generator makes its records of (rawpack.py --split's buckets, concatenated). A render
state is the pack's when its key is: rawpack.py state_key, which is the key the raw runtime looks a state up by. Per
owner, every distinct declared state (one per line, whichever GPU's directory or archive declares it) is one of:
  pack      its key is the pack's
  near      its key is the pack's once each constant is written at its type's size (the pack's line writes it shorter,
            so the device's key, written at full size, misses it)
  packable  not the pack's, and the runtime would look it up there (model.py packable: both its functions one
            framework's default.metallib, and every function it links a framework library's)
  creation  the runtime never looks it up in the pack (another library: a bundle's, an app's, a runtime-built one,
            or a linked function's), so the direct compile makes it when it is created
  compute   a compute state: the pack keys a kernel by its dispatch's threadgroup shape, which no descriptor names
  tile      a tile render state
  tessellation  a render state with tessellation, which its line does not carry
  unspecialized  a render state whose function lacks a constant its library requires (the archive does not record it)
--harvest: the device journals; "seen" counts the declared states the device made. --missing: the packable states'
lines, for the pack generator.
"""

import argparse
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

from drivers.TegraGM20BMetal.corpus.tools.archives.gpubin import type_width
from drivers.TegraGM20BMetal.corpus.tools.archives.model import packable
from drivers.TegraGM20BMetal.raw.pack.rawpack import state_key

CONSTANT = re.compile(r'(\S+):(\d+):([0-9a-f]*)(?= |$)')
COLUMNS = ('declared', 'device', 'pack', 'near', 'packable', 'creation', 'unspecialized', 'compute', 'tile')
COLUMNS += ('tessellation', 'seen')


def full_width(key: str) -> str:
    """Give the key with every constant's bytes padded to its type's size, as the device writes them."""

    def pad(m: re.Match[str]) -> str:
        width = type_width(int(m.group(2)))
        return f'{m.group(1)}:{m.group(2)}:{m.group(3).ljust(2 * width, "0")}' if width else m.group(0)

    return CONSTANT.sub(pad, key)


def compute_key(line: str) -> str:
    """Give a C line's kernel, library and constants."""
    return ' '.join(line.split(' | k ', 1)[-1].split()).rstrip(' |')


def keys(paths: list[Path]) -> tuple[set[str], set[str]]:
    """Give the render state keys and compute kernel keys of corpus files."""
    render, compute = set(), set()
    for path in paths:
        for line in path.read_text().splitlines():
            if line.startswith('R ok=1 '):
                render.add(state_key(line))
            elif line.startswith('C ok=1 '):
                compute.add(compute_key(line))
    return render, compute


def classify(kind: str, line: str, pack: set[str], near: set[str]) -> str:
    if kind in ('compute', 'tile', 'tessellation', 'unspecialized'):
        return kind
    key = state_key(line)
    if key in pack:
        return 'pack'
    if full_width(key) in near:
        return 'near'
    return 'packable' if packable(line) else 'creation'


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--inventory', type=Path, required=True)
    parser.add_argument('--pack', type=Path, required=True)
    parser.add_argument('--harvest', type=Path, action='append', default=[])
    parser.add_argument('--missing', type=Path)
    args = parser.parse_args()
    pack, _ = keys([args.pack])
    near = {full_width(k) for k in pack}
    seen_render, seen_compute = keys(args.harvest)
    table: dict[str, Counter[str]] = defaultdict(Counter)
    missing: set[str] = set()
    rows: dict[tuple[str, str, str], str] = {}
    for row in args.inventory.read_text().splitlines():
        kind, owner, _source, family, line = row.split('\t')
        if rows.get((kind, owner, line)) != 'device':
            rows[kind, owner, line] = family
    for (kind, owner, line), family in rows.items():
        bin_ = classify(kind, line, pack, near)
        seen = compute_key(line) in seen_compute if kind == 'compute' else state_key(line) in seen_render
        for counts in (table[owner], table['total']):
            counts['declared'] += 1
            counts['device'] += family == 'device'
            counts[bin_] += 1
            counts['seen'] += seen
        if bin_ == 'packable' and family == 'device':
            missing.add(line)
    width = max(map(len, table))
    print(f'{"owner":<{width}} ' + ' '.join(f'{c:>8}' for c in COLUMNS))
    for owner, counts in sorted(table.items(), key=lambda kv: (kv[0] == 'total', kv[0])):
        print(f'{owner:<{width}} ' + ' '.join(f'{counts[c]:>8}' for c in COLUMNS))
    if args.missing:
        args.missing.write_text(''.join(line + '\n' for line in sorted(missing)))
        print(f'{len(missing)} packable states the pack lacks: {args.missing}', file=sys.stderr)


if __name__ == '__main__':
    main()
