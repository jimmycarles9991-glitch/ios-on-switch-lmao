#!/usr/bin/env python3
"""What a storage form's program does with each colour it fetches: per colour and channel, on the host.

usage: storecheck.py <pack.bin> --corpus <R lines>... [--library <substring>] [--invocations N]

For every storage-form record of the corpus's states (those whose functions' library names the substring, RenderBox's
by default), each fetched channel's load is moved by DELTA and the program run again over the same quads (quad.py):
  unused  nothing the program stores or outputs moved
  kept    only its own store moved, by exactly DELTA, from a store equal to the load: the channel written back as read
  add     only its own store moved, within two fp32 ulps of DELTA: the store is the load plus a term that does not read
          it, what the ROP's ONE, ONE, ADD makes of a fetch-free program's output
  own     only its own store moved, otherwise (a product, a mix, a minimum, a load rounded to half before its sum)
  read    another store, an output or a kill moved
Loads are multiples of 2^-10, so a load moved by DELTA is exact and only the program's own sum rounds. Each set-0 word
that holds a fetched colour's handle is pinned to a word naming the colour, so a surface load or store
names its colour in every quad (a handle drawn at random named nothing across quads). A state can leave the interlock
on a colour only where each of its channels is unused, kept or add, and no other colour it stores is fetched.
"""

import argparse
import collections
import math
import random
import struct
import sys
from pathlib import Path

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'drivers/TegraGM20BMetal/raw/pack'))

import rawpack

from tools.gpu.maxwell import isa, nakstat, rungcheck
from tools.gpu.maxwell.interp import Inputs, UnsupportedError
from tools.gpu.maxwell.quad import LANES, run_quad

DELTA = 0.25
HANDLE = 0x5E7C0000  # a fetched colour's handle word: this, its colour in the low byte
ENTRY_FETCH = 3  # runtime/raw_pack.h EntryFetch
CBUF_SET = 3  # PackCbufType CbufSet
STORAGE_METADATA, SKIPPED = 8, 2 | 16 | 32  # PipelineStorageMetadata; barrier, variant and fetch-free records
# PackPipeline past its head: entry[32], pushMask, pushWords[32], blendColorWord, halfBlendColorWord, iterated
STORAGE_MASKS = 8 * 32 + 4 + 4 * 32 + 4
PROGRAM = struct.Struct('<IIIII')  # PackProgram: heapOffset, program, bytes, dataOffset, dataBytes


def f32(x: float) -> int:
    return struct.unpack('<I', struct.pack('<f', x))[0]


def value(word: int) -> float:
    return struct.unpack('<f', struct.pack('<I', word))[0]


class Record:
    """A storage-form record's fragment program and the set-0 words its fetched colours' handles are read from."""

    def __init__(self, pack: bytes, sections: list, at: int, head: struct.Struct, program_bytes: int):
        key, _, _, fs, _, _, entries, *_, self.flags = head.unpack_from(pack, at)
        self.key = key
        self.written = pack[at + head.size + STORAGE_MASKS + 1]
        program = sections[0][0] + program_bytes * fs
        heap, _, size, data, _ = PROGRAM.unpack_from(pack, program)
        cbufs, types, _, bound = nakstat.program_fields(pack, program, program_bytes)
        self.bound = bound - 1
        image = pack[sections[7][0] + heap + 0x30 : sections[7][0] + heap + (data or size)]
        self.code = image[isa.HEADER_BYTES :]
        self.outputs = rungcheck.nakstat_outputs(image)
        banks = [b for b in range(min(cbufs, len(types))) if types[b] & 7 == CBUF_SET]
        self.handles = {}
        for k in range(entries):
            offset, kind, _, slot = struct.unpack_from('<HBBB', pack, at + head.size + 8 * k)
            if kind == ENTRY_FETCH:
                self.handles.update({(bank, offset): slot for bank in banks})


def records(path: Path, keys: set[int]) -> list[Record]:
    pack = path.read_bytes()
    version, _, count, _ = struct.unpack_from('<4I', pack, 8)
    size, program_bytes, nsections, head = nakstat.LAYOUTS[version]
    sections = [struct.unpack_from('<QQ', pack, 24 + 16 * i) for i in range(nsections)]
    out = []
    for i in range(count):
        r = Record(pack, sections, sections[1][0] + size * i, head, program_bytes)
        if r.key in keys and r.flags & STORAGE_METADATA and not r.flags & SKIPPED and r.handles:
            out.append(r)
    return out


def ulp32(x: float) -> float:
    """Give an fp32 value's unit in the last place (a normal one's; the smallest normal's below them)."""
    return 2.0 ** (max(math.frexp(x)[1], -125) - 24)


def kind(changed: set, own: tuple, base: float, moved: float, load: float | None) -> str:
    """Give what moving the load of `own` (colour, channel) did, from what moved and its own store before and after."""
    if not changed:
        return 'unused'
    if changed != {own}:
        return 'read'
    if moved - base == DELTA and base == load:
        return 'kept'
    if abs((moved - base) - DELTA) <= 2 * ulp32(max(abs(base), abs(moved))):
        return 'add'
    return 'own'


class Quads:
    """One invocation's lanes of a record's program, its fetched colours' loads drawn once and moved on demand."""

    def __init__(self, r: Record, invocation: int):
        self.r, self.invocation = r, invocation
        self.seeded = rungcheck.Seeded(1, invocation, [0, 0, 0, 0], {})
        self.loads: dict[tuple[int, int], list[float]] = {}

    def inputs(self, lane: int, moved: tuple | None) -> Inputs:
        base = self.seeded.inputs(lane, self.r.bound)

        def cbuf(bank: int, offset: int) -> int:
            colour = self.r.handles.get((bank, offset))
            return HANDLE | colour if colour is not None else base.cbuf(bank, offset)

        def texture(op: tuple) -> list[int]:
            if op[0] != 'suld' or op[1] & ~0xFF != HANDLE:
                return base.texture(op)
            colour = op[1] & 0xFF
            rng = random.Random(repr((self.invocation, lane, colour)))
            load = self.loads.setdefault((lane, colour), [rng.randrange(-2048, 2048) / 1024 for _ in range(4)])
            return [f32(v + (DELTA if (colour, ch) == moved else 0.0)) for ch, v in enumerate(load)]

        return Inputs(base.attribute, cbuf, texture, base.bound_cbuf)

    def run(self, moved: tuple | None) -> list:
        return run_quad(self.r.code, [self.inputs(lane, moved) for lane in range(LANES)], self.r.outputs)


def stores(result: tuple) -> dict:
    """Give a lane's surface stores of fetched colours, by (colour, channel)."""
    return {(s[0] & 0xFF, k): w for s in result[2] if s[0] & ~0xFF == HANDLE for k, w in enumerate(s[2])}


def lane_kind(a: tuple, b: tuple, own: tuple, outputs: int, load: list[float] | None) -> str:
    """Give what moving `own`'s load did in one lane: `a` the lane's run before, `b` after."""
    if a[1] or b[1]:
        return 'read' if a[1] != b[1] else ''
    sa, sb = stores(a), stores(b)
    changed = {k for k in sa.keys() | sb.keys() if sa.get(k) != sb.get(k)}
    changed |= {('out', o) for o in range(outputs) if a[0][o] != b[0][o]}
    before, after = value(sa.get(own, 0)), value(sb.get(own, 0))
    return kind(changed, own, before, after, load[own[1]] if load else None)


def check(r: Record, invocations: int) -> dict[tuple[int, int], set[str]]:
    """Give each fetched (colour, channel) the kinds its quads showed."""
    found = collections.defaultdict(set)
    for invocation in range(invocations):
        quads = Quads(r, invocation)
        base = quads.run(None)
        for own in ((c, ch) for c in sorted(set(r.handles.values())) for ch in range(4)):
            moved = quads.run(own)
            for lane in range(LANES):
                verdict = lane_kind(base[lane], moved[lane], own, r.outputs, quads.loads.get((lane, own[0])))
                if verdict:
                    found[own].add(verdict)
    return found


def leaves(found: dict[tuple[int, int], set[str]], written: int) -> list[int]:
    """Give the colours whose every channel is unused, kept or add, where no other written colour reads its fetch."""
    colours = {c for c, _ in found}
    free = [c for c in colours if all(found[c, ch] <= {'unused', 'kept', 'add'} for ch in range(4))]
    return [c for c in free if not any(written >> o & 1 and o not in free for o in colours)]


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('pack', type=Path)
    ap.add_argument('--corpus', type=Path, action='append', default=[])
    ap.add_argument('--library', default='RenderBox.framework')
    ap.add_argument('--invocations', type=int, default=6)
    a = ap.parse_args()
    functions = {}
    for path in a.corpus:
        for line in path.read_text().splitlines():
            if line.startswith('R ok=1 ') and a.library in line:
                functions[nakstat.fnv(rawpack.state_key(line).encode())] = line.split('| f ')[1].split()[0]
    tally = collections.Counter()
    for r in records(a.pack, set(functions)):
        try:
            found = check(r, a.invocations)
        except UnsupportedError as e:
            tally[functions[r.key], f'unsupported {e}'] += 1
            continue
        colours = sorted({c for c, _ in found})
        shown = ' '.join(f'c{c}=' + ','.join('/'.join(sorted(found[c, ch])) for ch in range(4)) for c in colours)
        out = leaves(found, r.written)
        print(f'{r.key:016x} {functions[r.key]} written={r.written:#x} {shown} leaves={out or "-"}')
        tally[functions[r.key], 'leaves' if out else 'interlocked'] += 1
    for (function, verdict), n in sorted(tally.items()):
        print(f'{function}: {n} {verdict}')


if __name__ == '__main__':
    main()
