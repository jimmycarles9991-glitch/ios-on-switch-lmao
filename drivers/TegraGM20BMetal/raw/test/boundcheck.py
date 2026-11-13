#!/usr/bin/env python3
"""boundcheck: the raw path's words for a state's probe-bound draw against m2v's own, and its programs drawn over both.

directpack keeps m2v's instantiation of the probe draw that binds every buffer slot at its probe offset beside each
record (pack/probe_bind.h, runtime/RawDirect.h BoundDraw): its root push words and each stage's argument table. The raw
path's words for that draw are the record's (runtime/raw_gpu.cpp RenderEncoder::prepare): its push words, each buffer's
residual in its record word, and its argument tables with theirs (runtime/raw_spill_table.h composeSpill). They must
equal m2v's on every word m2v writes, but for those the raw path writes from the draw's own state: the dynamic blend
colour and fragment-test words, and the root table and ExecutionStatus addresses (holes, 0 in every probe draw). A
residual word agrees modulo its buffer's base alignment: m2v starts a descriptor at the function's window on the buffer,
the raw path at the bind offset rounded down (runtime/raw_residual.h), and each reads the bound address plus the word.

Each record with an argument table is drawn too: its vertex program for four vertices and its fragment program for one
quad on tools/gpu/maxwell's interpreter, once over the raw path's words and once over m2v's, with the same buffers,
textures and descriptors. Both runs of a stage must give the same output bits, a stage with a table must read it
through the record's entry, and some other word of it must change what it gives.

usage: boundcheck.py <records.bin>
"""

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'pack'))
sys.path.insert(0, os.path.join(HERE, '..', '..', '..', '..'))
import directlink  # noqa: E402
import packfile  # noqa: E402

from tools.gpu.maxwell import isa  # noqa: E402
from tools.gpu.maxwell.interp import Inputs, UnsupportedError  # noqa: E402
from tools.gpu.maxwell.quad import LANES, run_quad  # noqa: E402

ROOT_PUSH, ROOT_WORDS = 0x700, 32
BUFFERS, SPILL, TABLES, HEAP, ROOTS = 0x8000000, 0x7100000, 0x3000000, 0x4000000, 0x2000000
FRAGMENT_CODES = (1, 2, 3, 5, 6, 7, 9, 10, 11, 13, 14, 15)
V20_AT = (
    directlink.PIPELINE_BYTES - packfile.PIPELINE_V22.size - packfile.PIPELINE_V21.size - packfile.PIPELINE_V20.size
)
TAIL = V20_AT - packfile.PIPELINE_V18.size - packfile.PIPELINE_V15.size


def f32(x: float) -> int:
    return struct.unpack('<I', struct.pack('<f', x))[0]


def bind_offset(stage: int, slot: int) -> int:
    """probe_bind.h probeBindOffset: stage 1 vertex, 2 fragment."""
    return (
        16 * (12 + slot // 12) + FRAGMENT_CODES[slot % 12] if stage == 2 else 16 * (1 + slot // 3) + 4 * (1 + slot % 3)
    )


def buffer_at(stage: int, slot: int) -> int:
    return BUFFERS + (32 * stage + slot) * 0x10000


def pattern(va: int) -> int:
    """Give a word of memory nothing wrote: a float of its address, a code of an 8-bit target from its neighbours'.

    Fourteen of them sum inside the target's codes, so a read at another word draws another colour.
    """
    return f32((va >> 2) % 13 / 256)


def varying(at: int) -> int:
    """Give an attribute word: a float of its address between a quarter and three quarters, so products stay codes."""
    return f32(0.25 + (at >> 2) % 13 / 26)


def flat(key: tuple) -> list[int]:
    return [x for v in key for x in (flat(v) if isinstance(v, tuple) else [v] if isinstance(v, int) else [])]


def dynamic_words(p: bytes) -> set[int]:
    """Give the record words (0-31 root, 32 on the fragment table's) the raw path writes from a draw's own state."""
    blend, half, _ = struct.unpack_from('<BBH', p, TAIL - 56)
    depth, stencil = struct.unpack_from('<BB', p, TAIL - 16)
    v20 = packfile.PIPELINE_V20.unpack_from(p, V20_AT)
    out = set()
    for word, count in ((blend, 8), (half, 4), (depth, 1), (stencil, 1), *((w, 2) for w in v20[:4])):
        out.update(range(word - 1, word - 1 + count) if word else ())
    return out


def tables_of(rec: dict) -> list[list[int]]:
    v20 = packfile.PIPELINE_V20.unpack_from(rec['pipeline'], V20_AT)
    vertex, fragment = v20[26], v20[27]
    return [list(rec['spill'][:vertex]), list(rec['spill'][vertex : vertex + fragment])]


def entries(rec: dict) -> list[dict]:
    p = rec['pipeline']
    return directlink.entries_of(p, directlink.PIPELINE.unpack_from(p)[6], rec['samplers'], [])


def residual_words(rec: dict) -> dict[int, tuple[int, int, int]]:
    """Give each buffer entry's stage, record word (0-31 root, 32 on its table) and base alignment by set byte."""
    return {
        e['off']: (1 if e['stage'] == 'v' else 2, (e['residual'] - ROOT_PUSH) // 4, 16 if e['ssbo'] else 256)
        for e in entries(rec)
        if e['kind'] == 'ubo' and e['residual'] is not None
    }


def place(stage: int, word: int) -> tuple[int, int]:
    """Name a record word: (0, word) a root push word, (stage, word) one of that stage's table."""
    return (0 if word < ROOT_WORDS else stage, word)


def word_of(push: list, tables: list[list[int]], stage: int, word: int) -> int:
    return (push[word] or 0) if word < ROOT_WORDS else tables[stage - 1][word - ROOT_WORDS]


def raw_words(rec: dict) -> tuple[list, list[list[int]]]:
    """Give the raw path's root push words (None: none written) and argument tables for the bound draw.

    The dynamic words are the record's; the caller gives them the draw's state.
    """
    p = rec['pipeline']
    mask, *words = struct.unpack_from('<33I', p, directlink.PIPELINE.size + packfile.PACK_ENTRIES * 8)
    push = [words[i] if mask >> i & 1 else None for i in range(ROOT_WORDS)]
    tables = tables_of(rec)
    named = residual_words(rec)
    for e in entries(rec):
        if e['off'] not in named:
            continue
        stage, word, align = named[e['off']]
        residual = bind_offset(stage, e['slot']) & (align - 1)
        if word < ROOT_WORDS:
            push[word] = residual
        else:
            tables[stage - 1][word - ROOT_WORDS] = residual
    return push, tables


def compare(rec: dict) -> list[str]:
    """Give each word the raw path's bound draw and m2v's disagree on."""
    m2v = rec['bound']
    push, tables = raw_words(rec)
    dynamic = dynamic_words(rec['pipeline'])
    aligned = {place(stage, word): align for stage, word, align in residual_words(rec).values()}

    def same(at: tuple[int, int], a: int | None, b: int) -> bool:
        return a == b or (at in aligned and a is not None and (a - b) % aligned[at] == 0)

    out = [
        f'push word {i}: raw {push[i]} m2v {m2v["push"][i]:#x}'
        for i in range(ROOT_WORDS)
        if m2v['mask'] >> i & 1 and i not in dynamic and not same((0, i), push[i], m2v['push'][i])
    ]
    for k in range(2):
        theirs = m2v['spill'][k]
        if len(tables[k]) != len(theirs):
            out.append(f'stage {k + 1} table: raw {len(tables[k])} words, m2v {len(theirs)}')
            continue
        out += [
            f'stage {k + 1} table word {i}: raw {a:#x} m2v {b:#x}'
            for i, (a, b) in enumerate(zip(tables[k], theirs, strict=True))
            if not same((k + 1, ROOT_WORDS + i), a, b) and not (k == 1 and ROOT_WORDS + i in dynamic)
        ]
    return out


class Draw:
    """The memory, descriptors and constant banks both runs of a record read; only the push words and tables differ."""

    def __init__(self, rec: dict, push: list, tables: list[list[int]]) -> None:
        self.words = {}
        self.set0 = {}
        self.table_reads = [0, 0]
        self.push = [w or 0 for w in push]
        for k, table in enumerate(tables):
            for i, w in enumerate(table):
                self.words[SPILL + 0x1000 * k + 4 * i] = w
        named = residual_words(rec)
        for e in entries(rec):
            if e['kind'] == 'ubo':
                stage = 1 if e['stage'] == 'v' else 2
                offset = bind_offset(stage, e['slot'])
                if e['off'] in named:
                    residual = word_of(push, tables, stage, named[e['off']][1])
                    residual -= (residual & 0x80000000) << 1
                else:
                    residual = offset & (15 if e['ssbo'] else 255)
                base = buffer_at(stage, e['slot']) + offset - residual
                self.set0[e['off']] = [base & 0xFFFFFFFF, base >> 32, 0x400, 0]
            elif e['kind'] == 'spill_table':
                table = tables[e['stage'] - 1]
                self.set0[e['off']] = [SPILL + 0x1000 * (e['stage'] - 1), 0, (4 * len(table) + 255) & ~255, 0]
            elif e['kind'] in ('texture_table', 'sampler_table'):
                self.set0[e['off']] = [TABLES, 0, 0x10000, 0]
        v20 = packfile.PIPELINE_V20.unpack_from(rec['pipeline'], V20_AT)
        for word in v20[:4]:
            if word:
                self.push[word - 1], self.push[word] = ROOTS, 0

    def get(self, va: int) -> int:
        if SPILL <= va < SPILL + 0x2000:
            self.table_reads[(va - SPILL) // 0x1000] += 1
        return self.words.get(va, pattern(va))

    def cbuf(self, prog: dict, bank: int, offset: int) -> int:
        kind = prog['cbufs'][bank] if bank < len(prog['cbufs']) else 0
        if kind == 1:
            if ROOT_PUSH <= offset < ROOT_PUSH + 4 * ROOT_WORDS:
                return self.push[(offset - ROOT_PUSH) // 4]
            return 0
        if kind == 3:
            return self.set0.get(offset & ~15, [0] * 4)[(offset & 15) // 4]
        if kind == 11:
            return self.get(HEAP + offset)
        if kind & 7 == 5:
            if kind >> 3 << 4 not in self.set0:
                raise UnsupportedError(f'a uniform buffer at set byte {kind >> 3 << 4} that no entry binds')
            base = self.set0[kind >> 3 << 4]
            return self.get((base[0] | base[1] << 32) + offset)
        raise UnsupportedError(f'a constant buffer of type {kind}')

    def inputs(self, prog: dict, attribute) -> Inputs:  # noqa: ANN001  an attribute function
        def cbuf(bank: int, offset: int) -> int:
            return self.cbuf(prog, bank, offset)

        def texture(key: tuple) -> list[int]:
            if key[0] == 'ldg':
                (lo, hi), off = key[1], key[2]
                va = (lo | hi << 32) + (off - (1 << 24) if off & 1 << 23 else off)
                return [self.get(va + 4 * k) for k in range(4)]
            if key[0] == 'txq':
                return [4, 4, 1, 1]
            seed = sum((i + 1) * v for i, v in enumerate(flat(key[1:])))
            return [f32((seed + 37 * k) % 251 / 256) for k in range(4)]

        return Inputs(attribute, cbuf, texture, -1)

    def run(self, rec: dict, stage: int) -> list:
        """Give a stage's outputs: each vertex lane's attribute stores, or each fragment lane's colours, kill, stores.

        A fragment program of the storage form writes its attachments with SUST, so its stores are its colours.
        """
        prog = rec['programs'][stage - 1]
        if stage == 1:
            lanes = [
                self.inputs(
                    prog, lambda at, i=i: i if at == 0x2FC else 0 if at == 0x2F8 else varying(0x9000000 + 64 * i + at)
                )
                for i in range(LANES)
            ]
            return [s[2] for s in run_quad(prog['image'][isa.HEADER_BYTES :], lanes, 0)]
        lanes = []
        for i in range(LANES):
            fixed = {0x70: f32((i & 1) + 0.5), 0x74: f32((i >> 1) + 0.5), 0x78: f32(0.5), 0x7C: f32(1.0)}
            lanes.append(
                self.inputs(prog, lambda at, i=i, fixed=fixed: fixed.get(at, varying(0xA000000 + 64 * i + at)))
            )
        outputs = struct.unpack_from('<I', prog['image'], 576 // 8)[0].bit_count()
        return run_quad(prog['image'][isa.HEADER_BYTES :], lanes, outputs)


def table_changes(tables: list[list[int]], stage: int):  # noqa: ANN201  a generator of tables
    """Give the tables with one word of `stage`'s changed: to each small code a rule word holds, and inverted."""
    for i, w in enumerate(tables[stage - 1]):
        for v in (0, 1, 2, 3, w ^ 1, ~w & 0xFFFFFFFF):
            if v != w:
                other = [list(t) for t in tables]
                other[stage - 1][i] = v
                yield other


def draw(rec: dict, stage: int) -> str | None:
    """Run a stage over the raw path's words and over m2v's; None when both give the same bits, else what differs.

    A stage with an argument table must read it through the record's entry, and some other word of it must change
    what the stage gives.
    """
    m2v = rec['bound']
    push, tables = raw_words(rec)
    for i in dynamic_words(rec['pipeline']):
        if i < ROOT_WORDS:
            push[i] = m2v['push'][i]
        elif i - ROOT_WORDS < len(tables[1]):
            tables[1][i - ROOT_WORDS] = m2v['spill'][1][i - ROOT_WORDS]
    theirs = [m2v['push'][i] if m2v['mask'] >> i & 1 else None for i in range(ROOT_WORDS)]
    ours = Draw(rec, push, tables)
    out, reference = ours.run(rec, stage), Draw(rec, theirs, m2v['spill']).run(rec, stage)
    if out != reference:
        return "its outputs differ from its outputs over m2v's words"
    if tables[stage - 1]:
        if not ours.table_reads[stage - 1]:
            return 'it reads no word of its argument table'
        if not any(Draw(rec, push, other).run(rec, stage) != out for other in table_changes(tables, stage)):
            return 'no other argument table word changes what it gives'
    return None


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    made = [r for r in directlink.load_records(sys.argv[1])[0] if r['outcome'] == 0]
    failed = drawn = 0
    for rec in made:
        for line in compare(rec):
            print(f'FAIL {line}: {rec["key"][-100:]}')
            failed += 1
        if not any(tables_of(rec)):
            continue
        name = rec['key'].split('| v ')[-1][:100]
        drawn += 1
        for stage, label in ((1, 'vertex'), (2, 'fragment')):
            try:
                differs = draw(rec, stage)
            except UnsupportedError as e:
                differs = f'not drawn ({e})'
            print(f'{"FAIL" if differs else "ok  "} {label} drawn{": " + differs if differs else ""}: {name}')
            failed += bool(differs)
    print(
        f"boundcheck: {len(made)} records against m2v's bound draw, {drawn} with argument tables drawn, {failed} failed"
    )
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
