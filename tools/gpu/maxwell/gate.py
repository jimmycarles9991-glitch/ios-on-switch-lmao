#!/usr/bin/env python3
"""The codegen gate: two directcheck dumps' fragment programs run on the same inputs, outputs compared bit for bit.

usage: gate.py <before dump> <after dump> --corpus <R lines>... [--invocations N] [--seed S] [--after-variant N]
               [--zero bank:first-last]...
       gate.py --rungs <dump> --corpus <R lines>... [--invocations N] [--seed S]
           each fetch-free form of the dump with the ROP unit that stands in for its fetch against its storage form,
           counted per rung (rungcheck.py)

Every state of the corpora in both dumps runs N quads of each program (quad.py over interp.py's lanes) on the same
seeded attributes, constant buffers, texture and storage-image results (specials included: NaNs with payloads,
infinities, signed zeros, f32 denormals, values past half's range). A state passes when every lane kills alike and makes
the same storage stores, and every lane that is not killed writes the same output registers (a discarded pixel's
registers are written nowhere), compared as the codes GM20B's store makes of them where the state's attachment is a
unorm format (rop.py); it is unsupported when either program uses an instruction the interpreter does not run.
"""

import argparse
import hashlib
import json
import random
import struct
import sys
from pathlib import Path

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from tools.gpu.maxwell import isa, nakstat, rop, rungcheck
from tools.gpu.maxwell.interp import Inputs, UnsupportedError
from tools.gpu.maxwell.quad import LANES, run_quad

SPECIALS = [
    0x7FC00000,
    0x7FC12345,
    0xFFC00001,
    0x7F800000,
    0xFF800000,
    0x80000000,
    0x00000001,
    0x807FFFFF,
    0x477FF000,
    0x47800000,
    0xC7800000,
    0x3F800000,
    0x00800000,
    0x387FFFFF,
    # packed half pairs: -0, +-inf, NaN, denormals of both signs, the largest half, 1
    0x80008000,
    0x7C007C00,
    0xFC00FC00,
    0x7E00FE00,
    0x00010001,
    0x83FF83FF,
    0x7BFF7BFF,
    0x3C003C00,
]
# The share of words drawn from SPECIALS; the special pass draws most of them there
SPECIAL_SHARE, SPECIAL_HEAVY = 0.15, 0.7
OMAP_TARGET_BYTE = 576 // 8


def word(rng: random.Random, nice: float, special: float = SPECIAL_SHARE) -> int:
    r = rng.random()
    if special > SPECIAL_SHARE:
        nice = min(nice, 1 - special - 0.05)
    if r < nice:
        return struct.unpack('<I', struct.pack('<f', rng.uniform(-0.25, 1.25)))[0]
    if r < nice + special:
        return rng.choice(SPECIALS)
    if r < nice + special + 0.05:
        return rng.randrange(0, 64) * 4
    return rng.getrandbits(32)


class Seeded:
    """Inputs as pure functions of (seed, invocation, operands): the same for both programs."""

    def __init__(self, seed: int, invocation: int, pinned: dict | None = None, special: float = SPECIAL_SHARE):
        self.base = (seed, invocation)
        self.pinned = pinned or {}
        self.special = special

    def rng(self, *key: object) -> random.Random:
        # repr, not hash(): Python salts a str's hash per process, which would make a quad's number name other inputs
        # in every run
        return random.Random(int.from_bytes(hashlib.blake2b(repr((self.base, key)).encode(), digest_size=8).digest()))

    def inputs(
        self, lane: int = 0, bound_cbuf: int = -1, cbuf_map: tuple[int, ...] | None = None, data: tuple | None = None
    ) -> Inputs:
        """`data`: (the program's shader-data bank, its constant data's words by byte offset), which that bank reads."""

        def attribute(addr: int) -> int:
            return 0x3F800000 if addr == 0x7C else word(self.rng('a', addr, lane), 0.9, self.special)

        def cbuf(bank: int, offset: int) -> int:
            if data is not None and bank == data[0]:
                return data[1].get(offset & 0xFFFFFFFF, 0)
            if cbuf_map is not None:
                if not 0 <= bank < len(cbuf_map):
                    raise UnsupportedError(f'unmapped constant buffer {bank}')
                bank = cbuf_map[bank]
            seeded = word(self.rng('c', bank, offset & 0xFFFFFFFF), 0.6, self.special)
            pin = self.pinned.get((bank, offset))
            if isinstance(pin, tuple):
                return (seeded & ~pin[0] | pin[1]) & 0xFFFFFFFF
            return seeded if pin is None else pin

        def texture(operands: tuple) -> list[int]:
            if operands[0] == 'txq' and operands[2] in TXQ_SAMPLES:
                return txq_samples(operands[2], operands[3])
            r = self.rng('t', operands)
            return [word(r, 0.85, self.special) for _ in range(4)]

        return Inputs(attribute, cbuf, texture, bound_cbuf)


# TXQ's texture-type and sample-position queries, as GM20B answers them for a 4x multisampled surface: NAK's MS image
# addressing reads the sample count from one and, before 3da907153, the sample's place from the other; answered
# from the fixed layout, both forms of the program are judged on the same surface
TXQ_TEXTURE_TYPE, TXQ_SAMPLER_POS = 2, 5
TXQ_SAMPLES = (TXQ_TEXTURE_TYPE, TXQ_SAMPLER_POS)
MS_SAMPLES = 4


def sample_position(s: int) -> int:
    """Give TXQ SamplerPos of 4x sample `s`: (s & 1, s >> 1) in the 2x2 block, signed 4.12 about the centre."""
    x, y = ((s & 1) + 0.5) / 2 - 0.5, ((s >> 1 & 1) + 0.5) / 2 - 0.5
    return (int(x * 4096) & 0xFFFF) | (int(y * 4096) & 0xFFFF) << 16


def txq_samples(query: int, sample: int) -> list[int]:
    if query == TXQ_TEXTURE_TYPE:
        return [0, 0, MS_SAMPLES, 0]
    return [sample_position(sample & 3), 0, 0, 0]


SHADER_DATA = 2  # pack.h CbufShaderData


def constant_data(j: dict) -> tuple | None:
    """A dump's fragment program's constant data as (its shader-data bank, words by byte offset), None without one."""
    banks = j.get('fs_cbufs', [])
    if SHADER_DATA not in banks or not j.get('fs_data'):
        return None
    data = bytes.fromhex(j['fs_data'])
    return banks.index(SHADER_DATA), {4 * i: struct.unpack_from('<I', data, 4 * i)[0] for i in range(len(data) // 4)}


def bank_map(before: list[int], after: list[int]) -> tuple[int, ...] | None:
    """After's banks to before's by their types, where after binds the same types in another order; else None."""
    if before == after or sorted(before) != sorted(after) or len(set(before)) != len(before):
        return None
    return tuple(before.index(t) for t in after)


def run_pixel(image: bytes, quad: list[Inputs], widths: list | None = None, issued: list | None = None) -> list[tuple]:
    """Run a pixel program's quad; give each lane killed with its stores, or the components it writes and its stores.

    Where `widths` names a component's channel width, the component is the code the ROP stores (rop.store_code).
    `issued` gets the instructions the quad issued.
    """
    registers = rungcheck.written_registers(image)
    out = []
    for regs, killed, stores in run_quad(image[isa.HEADER_BYTES :], quad, rungcheck.nakstat_outputs(image), issued):
        if killed:
            out.append((killed, stores))
            continue
        written = [regs[r] for r in registers]
        if widths:
            written = [rop.store_code(v, w) if w else v for v, w in zip(written, widths, strict=False)]
        out.append((written, killed, stores))
    return out


def compare(  # noqa: PLR0913, PLR0917  the inputs' seed and pins, the state's attachments, the counts
    before: tuple,
    after: tuple,
    n: int,
    seed: int,
    pinned: dict | None = None,
    key: str = '',
    issued: tuple = (None, None),
    *,
    after_cbuf_map: tuple[int, ...] | None = None,
    special: float = SPECIAL_SHARE,
) -> str:
    """Compare two programs, each (image, the constant buffer its bound textures read handles from or -1[, its
    constant data: (shader-data bank, words by byte offset)]).


    `issued`: lists that get each quad's issued instructions, before's then after's.
    `after_cbuf_map`: after's physical banks to before's logical banks, validated against their binding metadata.
    """
    if after_cbuf_map is not None and sorted(after_cbuf_map) != list(range(len(after_cbuf_map))):
        raise ValueError('constant buffer map must be a permutation')
    if rungcheck.written_registers(before[0]) != rungcheck.written_registers(after[0]):
        return 'different: output maps'
    widths = rop.widths(key, struct.unpack_from('<I', before[0], OMAP_TARGET_BYTE)[0])

    def run(program: tuple, i: int, counts: list | None, cbuf_map: tuple[int, ...] | None = None) -> list:
        return run_pixel(
            program[0],
            [
                Seeded(seed, i, pinned, special).inputs(
                    lane, program[1], cbuf_map, program[2] if len(program) > 2 else None
                )
                for lane in range(LANES)
            ],
            widths,
            counts,
        )

    for i in range(n):
        try:
            a, b = run(before, i, issued[0]), run(after, i, issued[1], after_cbuf_map)
        except UnsupportedError as e:
            return f'unsupported: {e}'
        if a != b:
            lane = next(k for k in range(LANES) if a[k] != b[k])
            return f'different: quad {i} lane {lane}: {describe(a[lane], b[lane])}'
    return 'identical'


def compare_vertex(before: bytes, after: bytes, n: int, seed: int, special: float = SPECIAL_SHARE) -> str:
    """Compare two vertex programs by what each lane stores: its output attributes (AST) and any memory stores."""
    for i in range(n):
        quad = [Seeded(seed, i, None, special).inputs(lane) for lane in range(LANES)]
        try:
            a = [r[-1] for r in run_quad(before[isa.HEADER_BYTES :], quad, 0)]
            b = [r[-1] for r in run_quad(after[isa.HEADER_BYTES :], quad, 0)]
        except UnsupportedError as e:
            return f'unsupported: {e}'
        if a != b:
            lane = next(k for k in range(LANES) if a[k] != b[k])
            at = next((k for k in range(min(len(a[lane]), len(b[lane]))) if a[lane][k] != b[lane][k]), 0)
            return f'different: quad {i} lane {lane} store {at}'
    return 'identical'


def describe(a: tuple, b: tuple) -> str:
    """Where two lanes' results part: a kill, the output registers, or the first store that differs."""
    if a[0] is True or b[0] is True or a[0] != b[0]:
        name = lambda r: 'killed' if r[0] is True else str([hex(x) for x in r[0]])  # noqa: E731
        return f'{name(a)} vs {name(b)}'
    sa, sb = a[2], b[2]
    at = next((k for k in range(min(len(sa), len(sb))) if sa[k] != sb[k]), min(len(sa), len(sb)))
    store = lambda s: str(s[at]) if at < len(s) else 'none'  # noqa: E731
    return f'store {at}: {store(sa)} vs {store(sb)}'


def main() -> None:
    if '--rungs' in sys.argv:
        sys.argv.remove('--rungs')
        rungcheck.main()
        return
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('before', type=Path)
    ap.add_argument('after', type=Path)
    ap.add_argument('--corpus', type=Path, action='append', default=[])
    ap.add_argument('--invocations', type=int, default=200)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--after-variant', type=int, help="compare against the after dump's fragment variant N")
    ap.add_argument('--zero', action='append', default=[], help='bank:first-last, constant-buffer bytes read as 0')
    ap.add_argument(
        '--zero-residuals', action='store_true', help="read the after dump's uniform bind residual push words as 0"
    )
    a = ap.parse_args()

    residuals = {}
    types: dict[str, list[list[int]]] = {}

    def load(d: Path, variant: int | None = None) -> dict:
        out = {}
        for f in d.glob('*.json'):
            j = json.loads(f.read_text())
            types.setdefault(j['key'], []).append(j.get('fs_cbufs', []))
            residuals[j['key']] = {(0, offset): 0 for offset in j.get('residual_words', [])}
            bound = j.get('fs_bound_textures', 0) - 1
            if variant is None:
                out[j['key']] = (bytes.fromhex(j['fs']), bound, constant_data(j))
            elif (v := nakstat.fragment_variant(j, variant)) is not None:
                out[j['key']] = (bytes.fromhex(v['code']), v.get('bound_textures', 0) - 1)
        return out

    pinned = {}
    for z in a.zero:
        bank, span = z.split(':')
        first, last = (int(x, 0) for x in span.split('-'))
        pinned.update({(int(bank), offset): 0 for offset in range(first, last + 1, 4)})
    before, after = load(a.before), load(a.after, a.after_variant)
    cbufs = {key: bank_map(*found) for key, found in types.items() if len(found) == 2 and a.after_variant is None}
    failed = False
    for label, key in nakstat.labelled(a.corpus):
        if key not in before or key not in after:
            print(f'{label}: missing from a dump')
            continue
        pins = pinned | residuals[key] if a.zero_residuals else pinned
        banks = cbufs.get(key)
        verdict = compare(before[key], after[key], a.invocations, a.seed, pins, key, after_cbuf_map=banks)
        failed |= verdict.startswith('different')
        print(f'{label}: {verdict}')
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
