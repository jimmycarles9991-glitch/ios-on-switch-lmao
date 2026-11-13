#!/usr/bin/env python3
"""Fetch rungs 2 and 3 on the host: each fetch-free form with its ROP blend against the storage form's stored colour.

usage: rungcheck.py <dump dir> --corpus <R lines>... [--invocations N] [--seed S]

The dump is directcheck's DIRECTCHECK_DUMP of a corpus whose lines carry fetch-free tags (raw/pack/ca/fetchfree.py):
<dir>/<key hash>.json the storage form, <dir>/free/<key hash>.json its fetch-free form. Both programs run the same quads
(quad.py) on the same seeded attributes, constant buffers and texture samples, every texture an 8-bit premultiplied
image and the destination 8-bit premultiplied codes, as Core Animation's are. The storage form's stored texel is
converted to codes by the ROP's store rule; the fetch-free form's output goes through the unit that stands in for the
fetch: rung 2 the chip's 8-bit integer blender (tools/benchloop/blend_models.py chip, with the form's own factors),
rung 3 the iterated blend program the form names (raw/pack/iterated/emu.py, every pass rounded to half). A colour
matrix form's program and constants are those the GPU's macro makes of the matrix uniform both programs read
(fetchfree_link.py finds it as the pack does). The uniform buffers' bind residuals read as 0. Each state gets the
largest difference in codes over every written channel; rule fetch.blend allows 2 (raw_fetch_rungs.h).
"""

import argparse
import hashlib
import json
import math
import random
import struct
import sys
from collections import Counter
from pathlib import Path

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'drivers/TegraGM20BMetal/raw/pack/iterated'))
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'drivers/TegraGM20BMetal/raw/pack'))

import emu
import fetchfree_link
import programs_long
import table
from ucode import half_value

from tools.benchloop.blend_models import store
from tools.gpu.maxwell import isa, nakstat
from tools.gpu.maxwell.interp import Inputs, UnsupportedError
from tools.gpu.maxwell.quad import LANES, run_quad

OMAP_TARGETS = 576 // 8
PER_TARGET = 0x1E00  # SET_BLEND_PER_TARGET_SEPARATE_FOR_ALPHA(j), then its op, source and dest for colour and alpha
OP_ADD, OP_SUBTRACT, OP_REVERSE, OP_MIN, OP_MAX = 0x8006, 0x800A, 0x800B, 0x8007, 0x8008
BOUND = 2
FETCH_HANDLE = 0x7E5E0000  # a fetched colour's handle word: this, its colour in the low byte
ENTRY_FETCH = 3  # runtime/raw_pack.h EntryFetch
# a generic attribute's vec4 is seeded premultiplied (w the largest), as Core Animation's vertex colours are
GENERIC = 0x80


def f32(x: float) -> int:
    return struct.unpack('<I', struct.pack('<f', x))[0]


def value(word: int) -> float:
    return struct.unpack('<f', struct.pack('<I', word))[0]


def half_pair(rng: random.Random) -> int:
    lo, hi = (struct.unpack('<H', struct.pack('<e', rng.random()))[0] for _ in range(2))
    return lo | hi << 16


class Seeded:
    """Inputs as pure functions of (seed, quad, operands), alike for both programs, every colour premultiplied.

    Images are 8-bit premultiplied, and so is each generic attribute's vec4. The two programs bind a Metal buffer at
    constant banks and set-0 handles of their own, so a uniform is seeded by its buffer and offset (each program's bank
    of each buffer: `buffers`) and a sample by everything but its handle.
    """

    def __init__(self, seed: int, invocation: int, destination: list[int], pinned: dict[tuple[int, int], int]):
        self.base = (seed, invocation)
        self.destination = destination
        self.pinned = pinned

    def rng(self, *key: object) -> random.Random:
        return random.Random(int.from_bytes(hashlib.blake2b(repr((self.base, key)).encode(), digest_size=8).digest()))

    def inputs(
        self,
        lane: int,
        bound: int,
        buffers: dict[int, int] | None = None,
        fetches: dict[tuple[int, int], int] | None = None,
        blended: int = 0,
    ) -> Inputs:
        """Give a lane's inputs.

        `fetches`: the set-0 words that hold each fetched colour's handle, by (bank, byte), and their colour; a read of
        the blended colour is the destination, a read of another the colour's own texel in both programs, whether it is
        a storage form's surface load or a barrier form's texel load.
        """

        def attribute(addr: int) -> int:
            if addr < GENERIC:
                return 0x3F800000 if addr == 0x7C else f32(self.rng('a', addr, lane).random())
            alpha = self.rng('a', addr | 0xC, lane).random()
            return f32(alpha if addr & 0xC == 0xC else alpha * self.rng('a', addr, lane).random())

        def cbuf(bank: int, offset: int) -> int:
            if (bank, offset) in self.pinned:
                return self.pinned[bank, offset]
            if fetches and (bank, offset) in fetches:
                return FETCH_HANDLE | fetches[bank, offset]
            if buffers and bank in buffers:
                return half_pair(self.rng('u', buffers[bank], offset))
            return half_pair(self.rng('c', bank, offset))

        def texture(op: tuple) -> list[int]:
            if fetches and op[0] in ('suld', 'tld', 'tex') and op[1] & ~0xFF == FETCH_HANDLE:
                return self.fetched(op[1] & 0xFF, blended, lane)
            if op[0] == 'suld':
                return [f32(code / 255) for code in self.destination]
            return premultiplied(self.rng('t', op[0], op[2:]) if op[0] == 'tex' else self.rng('t', op))

        return Inputs(attribute, cbuf, texture, bound)

    def fetched(self, colour: int, blended: int, lane: int) -> list[int]:
        """Give a fetched colour's texel: the destination of the blended one, any other's its own."""
        if colour != blended:
            return premultiplied(self.rng('fetch', colour, lane))
        return [f32(code / 255) for code in self.destination]


def premultiplied(rng: random.Random) -> list[int]:
    alpha = rng.choice((0, 255, rng.randrange(256)))
    return [f32(rng.randrange(alpha + 1) / 255) for _ in range(3)] + [f32(alpha / 255)]


def destination(rng: random.Random) -> list[int]:
    alpha = rng.choice((0, 1, 255, rng.randrange(256)))
    return [rng.randrange(alpha + 1) for _ in range(3)] + [alpha]


def written_targets(image: bytes) -> list[int]:
    omap = struct.unpack_from('<I', image, OMAP_TARGETS)[0]
    return [c for c in range(8) if omap >> (4 * c) & 15]


def target_outputs(image: bytes, colour: int) -> tuple[int, int]:
    """Give where colour `colour`'s components start among the output registers and how many the program writes.

    Every colour the program writes takes a vec4 of registers whatever its omap nibble (NAK's copy_fs_outputs_nv): a
    colour after one written in fewer than four components does not start at the count of those components.
    """
    omap = struct.unpack_from('<I', image, OMAP_TARGETS)[0]
    first = 4 * sum(1 for c in written_targets(image) if c < colour)
    return first, (omap >> (4 * colour) & 15).bit_count()


def factor(code: int, channel: int, s: list[int], d: list[int]) -> int:
    """Give an OGL blend coefficient as the chip's blender reads it: a code of the 8-bit source or destination."""
    sa, da = s[3], d[3]
    return {
        0x4000: 0,
        0x4001: 255,
        0x4300: s[channel],
        0x4301: 255 - s[channel],
        0x4302: sa,
        0x4303: 255 - sa,
        0x4304: da,
        0x4305: 255 - da,
        0x4306: d[channel],
        0x4307: 255 - d[channel],
        0x4308: min(sa, 255 - da) if channel < 3 else 255,
    }[code]


def fixed_blend(blob: dict[int, int], colour: int, source: list[float], dest: list[int]) -> list[int]:
    """Give the chip's 8-bit blender's codes (blend_models.chip) with the form's per-target state, all four channels."""
    s = [store(x) for x in source]
    base = PER_TARGET + 0x20 * colour
    out = []
    for c in range(4):
        op, fs, fd = (blob.get(base + k, 0) for k in ((0x10, 0x14, 0x18) if c == 3 else (0x4, 0x8, 0xC)))
        if op in (OP_MIN, OP_MAX):
            out.append((min if op == OP_MIN else max)(s[c], dest[c]))
            continue
        first, second = s[c] * factor(fs, c, s, dest), dest[c] * factor(fd, c, s, dest)
        t = first - second if op == OP_SUBTRACT else second - first if op == OP_REVERSE else first + second
        out.append(min(255, max(0, math.floor(t / 255 + 0.5))))
    return out


def iterated_blend(program: object, source: list[float], dest: list[int]) -> list[int]:
    """Give the stored codes of the iterated blend program over the source and destination, each pass in half."""
    result = emu.run(program, source, [x / 255 for x in dest], fp16=True)
    return [store(x) for x in result]


def pipe(record: dict) -> dict:
    """Give a dump's uniform ranges, set-0 entries and fragment program as fetchfree_link.py reads a pack pipeline's."""
    entries = [
        {
            'kind': 'ubo' if kind == 1 else 'other',
            'stage': 'f' if stage == 2 else 'v',
            'slot': slot,
            'ssbo': bool(sk & 1),
        }
        for kind, stage, slot, sk, *_ in record.get('entries', [])
    ]
    uniform = b''.join(bytes(r) for r in record.get('uniform', []))
    return {'uniform': uniform, 'entries': entries, 'fs_image': bytes.fromhex(record['fs'])}


def matrix_words(stored: dict, free: dict) -> tuple[int, int, int, int] | None:
    """Give the colour matrix the storage form reads: (constant buffer bank, byte offset, fragment buffer, words)."""
    found = fetchfree_link.matrix_uniform(pipe(stored), pipe(free))
    if not found:
        return None
    return fetchfree_link.fragment_banks(pipe(stored))[found[0]], found[1], found[0], found[2]


def matrix_program(seeded: Seeded, at: tuple[int, int, int, int]) -> object:
    """Give the program and constants SWITCH_GM20B_MACRO_VCM_CONSTANTS makes of the matrix uniform's 11 arguments.

    A uniform of the rows alone is followed by a clamp pair of 0 (raw_gpu.cpp matrixConstants).
    """
    lanes = seeded.inputs(0, -1, {at[0]: at[2]})
    words = [lanes.cbuf(at[0], at[1] + 4 * k) for k in range(at[3])]
    words += [0] * (fetchfree_link.ROW_WORDS + fetchfree_link.CLAMP_WORDS - len(words))
    halves = [half_value(w >> s & 0xFFFF) for w in words for s in (0, 16)]
    rows = [halves[4 * j : 4 * j + 4] for j in range(5)]
    return programs_long.vcm(rows, halves[20], halves[21])


def check(stored: dict, free: dict, n: int, seed: int) -> tuple[str, Counter]:
    """Give a state's verdict and its channels' differences in codes, counted."""
    colour, rung = free['free_colour'], free['free_rung']
    storage_code, free_code = bytes.fromhex(stored['fs']), bytes.fromhex(free['fs'])
    first, count = target_outputs(free_code, colour)
    if count != 4:
        return f'unsupported: the form writes {count} components of colour {colour}', Counter()
    blob = dict(zip(free['blob'][0::2], free['blob'][1::2], strict=True))
    outputs = nakstat_outputs(free_code)
    pinned = {(0, word): 0 for word in stored.get('residual_words', []) + free.get('residual_words', [])}
    matrix = matrix_words(stored, free) if free['free_program'] == fetchfree_link.MATRIX else None
    if free['free_program'] == fetchfree_link.MATRIX and not matrix:
        return 'unsupported: no uniform buffer shows the colour matrix', Counter()
    differences: Counter = Counter()
    rng = random.Random(seed)
    for i in range(n):
        dest = destination(rng)
        seeded = Seeded(seed, i, dest, pinned)
        program = matrix_program(seeded, matrix) if matrix else table.PROGRAMS.get(free['free_program'])
        try:
            a = run_quad(storage_code[isa.HEADER_BYTES :], quad(seeded, stored, colour), 0)
            b = run_quad(free_code[isa.HEADER_BYTES :], quad(seeded, free, colour), outputs)
        except UnsupportedError as e:
            return f'unsupported: {e}', differences
        for lane in range(LANES):
            _, killed_a, stores = a[lane]
            regs, killed_b, _ = b[lane]
            if killed_a or killed_b:
                if killed_a != killed_b:
                    return f'different: quad {i} lane {lane} killed {killed_a} vs {killed_b}', differences
                continue
            if len(stores) != 1:
                return f'different: quad {i} lane {lane}: {len(stores)} stores', differences
            want = [store(value(w)) for w in stores[0][2][:4]]
            source = [value(w) for w in regs[first : first + 4]]
            fixed = rung == 2
            got = fixed_blend(blob, colour, source, dest) if fixed else iterated_blend(program, source, dest)
            differences.update(abs(x - y) for x, y in zip(got, want, strict=True))
    return verdict_of(differences), differences


def verdict_of(differences: Counter) -> str:
    if not differences:
        return 'uncompared: every lane of every quad was killed'
    worst = max(differences)
    return 'exact' if worst == 0 else f'within {worst}' if worst <= BOUND else f'beyond: {worst} codes'


def fetched(record: dict) -> dict[tuple[int, int], int]:
    """Give the set-0 words a dumped record's fragment program reads its fetched colours' handles from.

    (bank, byte) -> colour; empty for a dump that names no entry's byte.
    """
    out = {}
    for bank in record.get('fs_set_banks', []):
        for entry in record.get('entries', []):
            if len(entry) > 4 and entry[0] == ENTRY_FETCH:
                out[bank, entry[4]] = entry[2]
    return out


def quad(seeded: Seeded, record: dict, blended: int = 0) -> list[Inputs]:
    """Give a quad's inputs for a record's program: its bound textures' handles read from its own constant buffer."""
    buffers = {bank: slot for slot, bank in fetchfree_link.fragment_banks(pipe(record)).items()}
    bound, fetches = record.get('fs_bound_textures', 0) - 1, fetched(record)
    return [seeded.inputs(lane, bound, buffers, fetches, blended) for lane in range(LANES)]


def nakstat_outputs(image: bytes) -> int:
    return 4 * len(written_targets(image))


def written_registers(image: bytes) -> list[int]:
    """Give the output registers that hold a component the program writes (the rest of a colour's vec4 is undefined)."""
    omap = struct.unpack_from('<I', image, OMAP_TARGETS)[0]
    return [4 * k + c for k, t in enumerate(written_targets(image)) for c in range(4) if omap >> (4 * t + c) & 1]


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('dump', type=Path)
    ap.add_argument('--corpus', type=Path, action='append', default=[])
    ap.add_argument('--invocations', type=int, default=50)
    ap.add_argument('--seed', type=int, default=1)
    a = ap.parse_args()

    def load(d: Path) -> dict:
        return {j['key']: j for j in (json.loads(f.read_text()) for f in d.glob('*.json'))}

    stored, free = load(a.dump), load(a.dump / 'free')
    rungs: Counter = Counter()
    beyond = False
    for label, key in nakstat.labelled(a.corpus):
        if key not in free:
            continue
        if key not in stored:
            print(f'{label}: no storage form in the dump')
            continue
        if not any(entry[0] == ENTRY_FETCH for entry in stored[key]['entries']):
            # a state whose programs read no colour draws plain: it has no fetch for a rung to stand in for
            print(f'{label}: draws plain, no fetch')
            rungs[free[key]['free_rung'], 'plain'] += 1
            continue
        rung = free[key]['free_rung']
        verdict, differences = check(stored[key], free[key], a.invocations, a.seed)
        kind = verdict.split(':')[0].split(' ')[0]
        rungs[rung, kind] += 1
        beyond |= kind in ('beyond', 'different')
        shown = ' '.join(f'{d}:{n}' for d, n in sorted(differences.items()))
        program = free[key]['free_program'] or 'rop'
        print(f'{label}: rung {rung} ({program}) {verdict} [{shown}]')
    for (rung, kind), n in sorted(rungs.items()):
        print(f'rung {rung}: {n} {kind}')
    sys.exit(1 if beyond else 0)


if __name__ == '__main__':
    main()
