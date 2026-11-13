#!/usr/bin/env python3
"""argcheck: HW Test's argument-buffer states (metal.native.argument_encoder) as the raw path draws them, on the host.

Each state's record is the direct compile's (pack/directpack.mm), and its fragment program runs on tools/gpu/maxwell's
interpreter over a model of what the raw device binds for its draws: the root table and its identity resident set, the
handle tables dense by header, set 1 holding header i at element i (runtime/raw_heap_tables.h), set 0's descriptors
and the push words (runtime/raw_gpu.cpp RenderEncoder::prepare), an argument buffer's bytes and the buffers its
pointers name. A texture fetch answers its handle, so each check is the colour a pointer leads to or the texture and
sampler headers a fetch names.

usage: argcheck.py --rootdump <rootdump> <records.bin>
  rootdump: //drivers/TegraGM20BMetal:rootdump, the device's writer of a draw's root table (bazel run passes it)
  records.bin: directpack <nvk dylib> <cache> <any metallib> <corpus> records.bin, with PROBE_LABEL= and
  PROBE_LIB_ROOT=<root>, over a corpus of lines "R ok=1 label=x samples=1 topo=0 ds=0/0 c0=70:15:0:1:0:0:1:0:0
  lib=/System/Library/Frameworks/HWTest.framework/default.metallib | v quad_vertex | f <function>", one per
  argument_colour_fragment, argument_slot_fragment, argument_textures_fragment, argument_array_fragment,
  argument_nested_fragment and argument_spill_fragment, and one of "v spill_vertex | f tint_fragment", with
  hw_test_app/shaders/HWTest.metal's library at <root>/System/Library/Frameworks/HWTest.framework/default.metallib.
"""

import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'pack'))
sys.path.insert(0, os.path.join(HERE, '..', '..', '..', '..'))
import directlink  # noqa: E402
import packfile  # noqa: E402

from tools.gpu.maxwell import isa  # noqa: E402
from tools.gpu.maxwell.interp import Inputs  # noqa: E402
from tools.gpu.maxwell.quad import LANES, run_quad  # noqa: E402

TICS, TSCS = 8192, 1024
ROOTS, TABLES, HEAP, SET0, ARG, INNER, COLOUR = (
    0x2000000,
    0x3000000,
    0x4000000,
    0x5000000,
    0x6000000,
    0x6100000,
    0x7000000,
)
SINK, STATUS, IDENTITY = ROOTS + 96, ROOTS + 160, ROOTS + 64
SPILL, BUFFERS = 0x7100000, 0x8000000  # a draw's argument table in the upload ring, and buffers bound at its slots
DRAW_ROOTS = 0x2100000  # a draw's own root table (raw_gpu.cpp RenderEncoder::rootTable), in the upload ring
VA_START, VA_END = 0x1000, 1 << 39
TAG = 1 << 32
CONSTEXPR_TSC = 99


class Memory:
    def __init__(self) -> None:
        self.words = {}

    def put(self, va: int, data: bytes) -> None:
        data = data.ljust((len(data) + 3) & ~3, b'\0')
        for i in range(0, len(data), 4):
            self.words[va + i] = struct.unpack_from('<I', data, i)[0]

    def get(self, va: int) -> int:
        return self.words.get(va, 0)


V20_AT = (
    directlink.PIPELINE_BYTES - packfile.PIPELINE_V22.size - packfile.PIPELINE_V21.size - packfile.PIPELINE_V20.size
)


def f32(x: float) -> int:
    return struct.unpack('<I', struct.pack('<f', x))[0]


def tail(rec: dict) -> tuple:
    return packfile.PIPELINE_V20.unpack_from(rec['pipeline'], V20_AT)


def device(mem: Memory, rec: dict, textures: dict, samplers: dict, holds) -> None:  # noqa: ANN001  a class predicate
    """Write the device's memory as raw_heap_tables.h does once the IDs of `textures` and `samplers` were asked for.

    Each maps a header to its entry's flags: a texture's handle_entry_flags, a sampler's SamplerWord.
    """
    identity = struct.pack('<4Q', VA_START, VA_START, VA_END - VA_START, 0)
    mem.put(ROOTS, bytes(32) + struct.pack('<4Q', SINK, 2, 1, 0) + identity)
    for i in range(1, TICS):
        mem.put(TABLES + 16 * i, struct.pack('<4I', i, 1, i if i in textures else 0, textures.get(i, 0)))
    for i in range(1, TSCS):
        mem.put(
            TABLES + 16 * TICS + 16 * i, struct.pack('<4I', i << 20, 1, i if i in samplers else 0, samplers.get(i, 0))
        )
    v20 = tail(rec)
    for k in range(v20[6]):
        heap_class, offset = v20[8 + 2 * k], v20[9 + 2 * k]
        if heap_class & 3 == 2:
            for tsc in samplers:
                mem.put(HEAP + offset + 4 * tsc, struct.pack('<I', tsc << 20))
        else:
            for tic in textures:
                mem.put(HEAP + offset + 4 * tic, struct.pack('<I', tic if holds(heap_class) else 0))


def draw_roots(mem: Memory, rec: dict, slots: dict, rootdump: str) -> None:
    """Write the fragment root table of a draw binding `slots` ({slot: (VA, length, offset)}) with the device's writer.

    The table lies above the identity entry it names, so the sink's R0 is the wrapped one (m2v resident_fields).
    """
    bound = [f'{n}={va}:{length}:{offset}' for n, (va, length, offset) in slots.items()]
    args = [rootdump, str(DRAW_ROOTS), str(tail(rec)[5]), str(SINK), str(IDENTITY), *bound]
    lines = subprocess.run(args, check=True, capture_output=True, text=True).stdout.split('\n')
    entries = [[int(w, 16) for w in line.split()] for line in lines if line]
    mem.put(DRAW_ROOTS, b''.join(struct.pack('<4Q', *e) for e in entries))


def bind_buffers(rec: dict, entries: list[dict], bound: dict, mem: Memory) -> tuple[dict, dict]:
    """Give the root push words and set-0 descriptors RenderEncoder::prepare writes for fragment slots `bound`.

    `bound` is {slot: address}; the words come by word, the descriptors by set byte. Each descriptor is at the address
    less its residual, the residual in the record's word: a root push word, or past them a word of the fragment argument
    table, which holds the record's words (raw_spill_table.h composeSpill).
    """
    v20 = tail(rec)
    words = list(rec.get('spill', [])[v20[26] : v20[26] + v20[27]])
    push, set0 = {}, {}
    for e in entries:
        if e['kind'] != 'ubo' or e['stage'] != 'f' or e['slot'] not in bound:
            continue
        address = bound[e['slot']]
        residual = address & (15 if e['ssbo'] else 255)
        set0[e['off']] = [(address - residual) & 0xFFFFFFFF, (address - residual) >> 32, 0x10000, 0]
        if e['residual'] is not None:
            word = (e['residual'] - 0x700) // 4
            if word < 32:
                push[word] = residual
            else:
                words[word - 32] = residual
    for e in entries:
        if e['kind'] == 'spill_table' and e['stage'] == 2:
            set0[e['off']] = [SPILL, 0, (4 * len(words) + 255) & ~255, 0]
            mem.put(SPILL, struct.pack(f'<{len(words)}I', *words))
    return push, set0


def run(rec: dict, mem: Memory, pixel: tuple, bound: dict | None = None) -> tuple[list[float], list[tuple]]:  # noqa: C901
    """Run the fragment program at `pixel`: give lane 0's colour and the (texture, sampler header) of each fetch.

    `bound`: the fragment buffer slots a draw binds besides the argument buffer, {slot: address} (bind_buffers).
    """
    p = rec['pipeline']
    hdr = directlink.PIPELINE.unpack_from(p)
    entries = directlink.entries_of(p, hdr[6], rec['samplers'], [])
    _, *words = struct.unpack_from('<33I', p, directlink.PIPELINE.size + packfile.PACK_ENTRIES * 8)
    v20 = tail(rec)
    push = list(words)
    for k in range(2):
        for word, va in ((v20[k], DRAW_ROOTS if v20[4 + k] else ROOTS), (v20[2 + k], STATUS)):
            if word:
                push[word - 1], push[word] = va & 0xFFFFFFFF, va >> 32
    set0 = {}
    for e in entries:
        if e['kind'] == 'ubo' and e['stage'] == 'f':
            set0[e['off']] = [ARG, 0, 256, 0]
        elif e['kind'] == 'texture_table':
            set0[e['off']] = [TABLES, 0, 16 * TICS, 0]
        elif e['kind'] == 'sampler_table':
            set0[e['off']] = [TABLES + 16 * TICS, 0, 16 * TSCS, 0]
        elif e['kind'] == 'handle' and e['smp'] and e['smp'][0] == 'constexpr':
            set0[e['off']] = [CONSTEXPR_TSC << 20, 0, 0, 0]
    if bound:
        words, descriptors = bind_buffers(rec, entries, bound, mem)
        for word, value in words.items():
            push[word] = value
        set0.update(descriptors)
    prog = rec['programs'][1]
    fetched = []

    def cbuf(bank: int, offset: int) -> int:
        kind = prog['cbufs'][bank] if bank < len(prog['cbufs']) else 0
        if kind == 1:
            if 0x700 <= offset < 0x780:
                return push[(offset - 0x700) // 4]
            sets = {0x38: [SET0, 0, 256, 0], 0x48: [HEAP, 0, v20[7], 0]}
            return sets.get(offset & ~15, [0] * 4)[(offset & 15) // 4]
        if kind == 3:
            return set0.get(offset & ~15, [0] * 4)[(offset & 15) // 4]
        if kind == 11:
            return mem.get(HEAP + offset)
        if kind & 7 == 5:
            base = set0.get(kind >> 3 << 4, [ARG, 0])
            return mem.get((base[0] | base[1] << 32) + offset)
        raise SystemExit(f'argcheck: a constant buffer of type {kind} at slot {bank}')

    def texture(key: tuple) -> list[int]:
        if key[0] == 'ldg':
            (lo, hi), off = key[1], key[2]
            va = (lo | hi << 32) + (off - (1 << 24) if off & 1 << 23 else off)
            return [mem.get(va + 4 * k) for k in range(4)]
        if key[0] in ('tex', 'tld', 'txq'):
            if key[0] != 'txq':
                fetched.append((key[1] & 0xFFFFF, key[1] >> 20))
            return [4, 4, 1, 1] if key[0] == 'txq' else [f32(0.5)] * 4
        raise SystemExit(f'argcheck: a fetch {key[0]}')

    def attribute(addr: int) -> int:
        return {0x70: f32(pixel[0] + 0.5), 0x74: f32(pixel[1] + 0.5), 0x78: f32(0.5), 0x7C: f32(1.0)}.get(addr, 0)

    outputs = struct.unpack_from('<I', prog['image'], 576 // 8)[0].bit_count()
    quad = [Inputs(attribute, cbuf, texture, -1) for _ in range(LANES)]
    regs = run_quad(prog['image'][isa.HEADER_BYTES :], quad, outputs)[0][0]
    return [struct.unpack('<f', struct.pack('<I', r))[0] for r in regs[:4]], fetched


def pointers(recs: dict, check, rootdump: str) -> None:  # noqa: ANN001  main's check
    """Check a colour behind an argument buffer's pointer, and a buffer slot reached through a draw's own root table."""
    mem = Memory()
    rec = recs['argument_colour_fragment']
    device(mem, rec, {}, {}, lambda _: True)
    mem.put(ARG, struct.pack('<Q', COLOUR))
    mem.put(COLOUR, struct.pack('<4f', 1, 0, 1, 1))
    out, _ = run(rec, mem, (4, 4))
    check('a colour behind a pointer', out == [1, 0, 1, 1], out)
    mem = Memory()
    rec = recs['argument_slot_fragment']
    device(mem, rec, {}, {}, lambda _: True)
    check('a slot in the fragment root table', tail(rec)[5] == 1 << 1, hex(tail(rec)[5]))
    mem.put(ARG, struct.pack('<Q', COLOUR))
    mem.put(COLOUR, struct.pack('<4f', 1, 0, 1, 1))
    mem.put(INNER + 32, struct.pack('<4f', 0, 0, 1, 1))
    draw_roots(mem, rec, {1: (INNER, 64, 32)}, rootdump)
    for x, want in ((0, [0, 0, 1, 1]), (1, [1, 0, 1, 1])):
        out, _ = run(rec, mem, (x, 0))
        check(f'pixel {x} of a slot or a pointer', out == want, out)


def handles(recs: dict, check, flags: int) -> None:  # noqa: ANN001  main's check
    """Check textures and samplers by ID, headers past the probe's own tables up to the pools' last, under `flags`."""
    mem = Memory()
    rec = recs['argument_textures_fragment']
    device(mem, rec, {27: flags, 8191: flags}, {1023: 0}, lambda _: True)
    mem.put(ARG, struct.pack('<3Q', TAG | 27, TAG | 8191, TAG | 1023 << 20))
    for x, tic in ((1, 27), (6, 8191)):
        _, fetched = run(rec, mem, (x, 4))
        own = (tic, 1023) in fetched and all(t == tic for t, _ in fetched)
        check(f'texture {tic} through sampler 1023 at x {x}, flags {flags:#x}', own, fetched[-1:])
    mem = Memory()
    rec = recs['argument_array_fragment']
    device(mem, rec, {4095: flags, 4096: flags, 8190: flags}, {16: 0, 1022: 0x40}, lambda _: True)
    mem.put(ARG, struct.pack('<5Q', TAG | 4095, TAG | 4096, TAG | 8190, TAG | 16 << 20, TAG | 1022 << 20))
    for x, tic in enumerate((4095, 4096, 8190)):
        for y, tsc in ((0, 16), (1, 1022)):
            _, fetched = run(rec, mem, (x, y))
            check(f'array texture {tic} through sampler {tsc}, flags {flags:#x}', (tic, tsc) in fetched, fetched[-1:])
    mem = Memory()
    rec = recs['argument_nested_fragment']
    device(mem, rec, {5000: flags}, {}, lambda c: c == 0x5)
    mem.put(ARG, struct.pack('<Q', INNER))
    mem.put(INNER, struct.pack('<2Q', TAG | 5000, COLOUR))
    mem.put(COLOUR, struct.pack('<4f', 0, 1, 1, 1))
    _, fetched = run(rec, mem, (0, 0))
    want = (5000, CONSTEXPR_TSC)
    check(f"a nested buffer's texture through its constexpr sampler, flags {flags:#x}", want in fetched, fetched[-1:])
    out, _ = run(rec, mem, (1, 0))
    check(f"a nested buffer's colour pointer, flags {flags:#x}", out == [0, 1, 1, 1], out)


def spills(recs: dict, check) -> None:  # noqa: ANN001  main's check
    """Check fourteen fragment buffers bound off their alignment beside an argument buffer.

    The first offsets' residuals lie in root words m2v's plan names where the probe's codes are shared, the last in the
    fragment stage's argument table; the vertex stage's table as its record names it.
    """
    rec = recs['argument_spill_fragment']
    v20 = tail(rec)
    check('a fragment argument table', v20[25] != 0 and v20[27] != 0, v20[24:29])
    mem = Memory()
    device(mem, rec, {}, {}, lambda _: True)
    mem.put(ARG, struct.pack('<Q', COLOUR))
    mem.put(COLOUR, struct.pack('<4f', 0, 0, 1, 1))
    bound = {}
    for slot in range(1, 15):
        va = BUFFERS + slot * 0x10000
        mem.put(va, struct.pack('<f', 1) * 1024)
        bound[slot] = va + 512 + 16 * slot + 4
        mem.put(bound[slot], struct.pack('<f', 1 / 32))
    entries = directlink.entries_of(rec['pipeline'], directlink.PIPELINE.unpack_from(rec['pipeline'])[6], [], [])
    spilled = [e['slot'] for e in entries if e['kind'] == 'ubo' and e['residual'] and e['residual'] >= 0x780]
    check('bind offsets in the fragment argument table', bool(spilled), spilled)
    out, _ = run(rec, mem, (0, 0), bound)
    # an 8-bit unorm target's channels rounded as Metal rounds them, in the program (the table's colour write rule)
    want = [112 / 255, 0, 1, 1]
    check(
        'fourteen buffers off 16, the spilled three by the argument table',
        all(abs(a - b) < 1e-6 for a, b in zip(out, want, strict=True)),
        out,
    )
    vertex = tail(recs['tint_fragment'])
    check('a vertex argument table', vertex[24] != 0 and vertex[26] != 0, vertex[24:29])


def main() -> None:
    if len(sys.argv) != 4 or sys.argv[1] != '--rootdump':
        raise SystemExit(__doc__)
    rootdump = os.path.abspath(sys.argv[2])
    made = (r for r in directlink.load_records(sys.argv[3])[0] if r['outcome'] == 0)
    recs = {r['key'].split('| f ')[1].split()[0]: r for r in made}
    checks = []

    def check(what: str, ok: bool, detail: object) -> None:
        checks.append(ok)
        print(f'{"ok  " if ok else "FAIL"} {what}: {detail}')

    pointers(recs, check, rootdump)
    spills(recs, check)
    # texel class indices, and the repeat sampler's SamplerWord (address mode 2 in bits 5-7)
    for flags in (0, 1 << 16, 0x25 << 16):
        handles(recs, check, flags)
    mem = Memory()
    rec = recs['argument_textures_fragment']
    device(mem, rec, {}, {1023: 0}, lambda _: True)
    mem.put(ARG, struct.pack('<3Q', TAG | 27, TAG | 8191, TAG | 1023 << 20))
    _, fetched = run(rec, mem, (1, 4))
    check('a texture no ID names reads header 0', fetched and all(t == 0 for t, _ in fetched), fetched[-1:])
    print(f'argcheck: {len(checks)} checks, {checks.count(False)} failed')
    sys.exit(1 if False in checks else 0)


if __name__ == '__main__':
    main()
