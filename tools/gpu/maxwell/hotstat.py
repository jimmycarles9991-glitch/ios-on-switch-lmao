#!/usr/bin/env python3
"""Every program of a pack's states, by stage: instructions, registers, occupancy, stalls, cycles and opcode mix.

usage: hotstat.py <pack dir> --corpus <hot corpus> [--json <out.json>] [--before <before.json>] [--top N]
                  [--dis <state hash prefix>]

<pack dir> holds a linked pack's pack.json and pack.bin (//tools/gpu/maxwell:hot_pack, //gpu/seed:pack).
A bundle's states, which the pack keys by their functions' AIR, are counted apart and not read. Each R line of the
corpus names a state's vertex and fragment programs, each D line a kernel. A program shared by several states counts
once in the totals. --before compares with an earlier --json, program by program (matched by state and stage): the
top N by the before side's cycles, then the totals.
"""

import argparse
import json
import re
import struct
import sys
from collections import Counter
from pathlib import Path

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from tools.gpu.maxwell import cost, isa, nakstat

HEADER = 80
# A bundle's library or a framework's resource library: the pack keys its states by their functions' AIR
AIR_KEYED = re.compile(r' lib=/System/Library/\S+?\.(?:bundle|framework/Resources)/')
BRANCHES = ('SSY', 'SYNC', 'BRA', 'BRK', 'PBK', 'CONT', 'PCNT', 'KIL', 'EXIT')
COMPARES = ('ISETP', 'FSETP', 'PSETP', 'HSETP2', 'DSETP')
MIX = (
    'NOP',
    'F2F',
    'MOV32I',
    'MOV',
    'SEL',
    'LDC',
    'LDG',
    'XMAD',
    'IMAD',
    'LOP',
    'LOP3',
    'IADD3',
    'IADD',
    'SHL',
    'SHR',
    'BFE',
    'BFI',
    'PRMT',
    'I2F',
    'F2I',
    'MUFU',
    'HADD2',
    'HMUL2',
    'HFMA2',
    'FMNMX',
    'TEX',
    'TEXS',
    'TLDS',
    'IPA',
    'LDL',
    'STL',
    'SULD',
    'SUST',
)


def mnemonic(word: int) -> str:
    return isa.name(word).split('_', 1)[0].removesuffix('32I') + ('32I' if isa.name(word).startswith('MOV32I') else '')


def measure(image: bytes, gprs: int) -> dict:
    code = image[HEADER:]
    names = [mnemonic(w) for w in isa.instructions(code)]
    mix = Counter(names)
    p = cost.program(code)
    return {
        'instructions': len(names),
        'gprs': gprs,
        'warps': nakstat.warps_per_sm(gprs),
        'cycles': round(p['cycles'], 1),
        'stalls': p['stalls'],
        'branches': sum(mix[b] for b in BRANCHES),
        'compares': sum(mix[c] for c in COMPARES),
        'mix': dict(mix),
    }


def label_of(line: str) -> str:
    head, *stages = line.split(' | ')
    label = next((w.split('=', 1)[1] for w in head.split() if w.startswith('label=')), '-')
    label = label.removeprefix('com.apple.coreanimation.')
    functions = '+'.join(s.split()[1] for s in stages if s[:2] in ('v ', 'f ', 'k ') and len(s.split()) > 1)
    return functions if label in ('-', '') else f'{label} {functions}'


def programs_of(pack: dict, corpus: Path) -> tuple[dict, int, int]:
    """Give each corpus state's default record's programs and the matched and named state counts.

    A program row is the stage's generic program, then its assumption variants (`@fs.v<i>`), which the device draws
    first where their assumptions hold; a state's other forms (fetch-free, barrier, a test class's variant record) are
    rows of their own (`@fs/free`, `@fs/barrier`, `@fs | tests=1`).
    """
    records: dict[str, list[dict]] = {}
    for p in pack['pipelines']:
        records.setdefault(p['key'], []).append(p)
    kernels = {k['key']: k for k in pack.get('kernels', [])}
    out, named, matched = {}, 0, 0
    for line in corpus.read_text().splitlines():
        if line.startswith('R ') and AIR_KEYED.search(line):
            continue
        if line.startswith('R '):
            named += 1
            key = nakstat.state_key(line)
            found = [p for k, recs in records.items() if k == key or k.startswith(key + ' | ') for p in recs]
            matched += any(p['key'] == key and not p['form'] for p in found)
            for p in found:
                suffix = p['key'][len(key) :] + (f'/{p["form"]}' if p['form'] else '')
                stages = [('vs', p['vs'])] if not suffix else []
                stages += [('fs', p['fs'])] if p['fs'] != 0xFFFFFFFF else []
                for stage, index in stages:
                    out[f'{key} @{stage}{suffix}'] = (label_of(line), stage, index, nakstat.fnv(key.encode()))
                fs_variants = [v for v in p.get('variants', []) if v['stage'] == 1]
                for i, v in enumerate(fs_variants):
                    out[f'{key} @fs.v{i}{suffix}'] = (label_of(line), 'fs', v['program'], nakstat.fnv(key.encode()))
        elif line.startswith('D '):
            named += 1
            name = line.split(' | k ')[1].split()[0] if ' | k ' in line else None
            hits = [key for key in kernels if name and f' {name}' in key]
            matched += bool(hits)
            for key in hits:
                out.setdefault(f'{key} @cs', (label_of(line), 'cs', kernels[key]['program'], nakstat.fnv(key.encode())))
    return out, matched, named


class Heap:
    """A pack.bin's shader heap: each program's header and code at its slot (pack.h PackProgram, PackSectionHeap)."""

    SECTION = 7
    HEADER_AT = 0x30

    def __init__(self, path: Path):
        self.data = path.read_bytes()
        self.start = struct.unpack_from('<Q', self.data, 24 + 16 * self.SECTION)[0]

    def image(self, program: dict) -> bytes:
        at = self.start + program['heap_offset'] + self.HEADER_AT
        return self.data[at : at + HEADER + program['code']]


def collect(pack_dir: Path, corpus: Path) -> dict:
    pack = json.loads((pack_dir / 'pack.json').read_text())
    programs = pack['programs']
    heap = Heap(pack_dir / 'pack.bin')
    rows = {}
    found, matched, named = programs_of(pack, corpus)
    for key, (label, stage, index, state_hash) in found.items():
        p = programs[index]
        image = heap.image(p)
        rows[key] = {
            'label': label,
            'stage': stage,
            'program': index,
            'hash': f'{state_hash:016x}',
            **measure(image, p['gprs']),
        }
    print(f'hotstat: {matched} of {named} corpus states matched a default record, {len(rows)} program rows')
    if named and matched < 0.9 * named:
        sys.exit('hotstat: under 90% of the corpus matched: the key rule and the pack disagree')
    return rows


def totals(rows: dict) -> dict:
    seen, out = set(), Counter()
    for r in rows.values():
        if r['program'] in seen:
            continue
        seen.add(r['program'])
        out[f'{r["stage"]}_programs'] += 1
        out[f'{r["stage"]}_gprs'] += r['gprs']
        for k in ('instructions', 'cycles', 'stalls', 'branches', 'compares'):
            out[f'{r["stage"]}_{k}'] += r[k]
        for name in MIX:
            out[f'{r["stage"]}_{name}'] += r['mix'].get(name, 0)
    return dict(out)


def pair(before: dict | None, b: dict, a: dict, field: str) -> str:
    return f'{a[field]}' if before is None else f'{b[field]}->{a[field]}'


def mix_pair(before: dict | None, b: dict, a: dict, name: str) -> str:
    after = a['mix'].get(name, 0)
    return f'{after}' if before is None else f'{b["mix"].get(name, 0)}->{after}'


def show(rows: dict, before: dict | None, top: int) -> None:
    ranked = sorted(rows, key=lambda k: -(before.get(k, rows[k]) if before else rows[k])['cycles'])
    print(f'{"program":70} {"instr":>11} {"gpr":>7} {"warps":>7} {"cycles":>15} {"stalls":>11} {"nop":>9} {"f2f":>9}')
    for key in ranked[:top]:
        a = rows[key]
        b = (before or {}).get(key, a)
        name = f'{a["hash"][:8]} {a["label"]} {key.rsplit(" @", 1)[1]}'
        print(
            f'{name[:70]:70} {pair(before, b, a, "instructions"):>11} {pair(before, b, a, "gprs"):>7} '
            f'{pair(before, b, a, "warps"):>7} {pair(before, b, a, "cycles"):>15} {pair(before, b, a, "stalls"):>11} '
            f'{mix_pair(before, b, a, "NOP"):>9} {mix_pair(before, b, a, "F2F"):>9}'
        )
    if before:
        common = {k: rows[k] for k in rows if k in before}
        tb, ta = totals({k: before[k] for k in common}), totals(common)
        print(f'totals over {len(common)} programs both packs hold:')
        for k in sorted(ta):
            print(f'  {k:28} {tb.get(k, 0):>10} -> {ta[k]:<10}')
    else:
        for k, v in sorted(totals(rows).items()):
            print(f'  {k:28} {v}')


def disassemble(pack_dir: Path, corpus: Path, prefix: str) -> None:
    pack = json.loads((pack_dir / 'pack.json').read_text())
    found, _, _ = programs_of(pack, corpus)
    for key, (label, _stage, index, state_hash) in found.items():
        if not f'{state_hash:016x}'.startswith(prefix):
            continue
        p = pack['programs'][index]
        image = Heap(pack_dir / 'pack.bin').image(p)
        print(f'== {label} {key.rsplit(" @", 1)[1]} program {index}, {p["gprs"]} GPRs')
        for i, word in enumerate(isa.instructions(image[HEADER:])):
            print(f'{i:5} {isa.name(word):14} {word:016x}')


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('pack', type=Path)
    ap.add_argument('--corpus', type=Path, required=True)
    ap.add_argument('--json', type=Path)
    ap.add_argument('--before', type=Path)
    ap.add_argument('--top', type=int, default=30)
    ap.add_argument('--dis')
    a = ap.parse_args()
    if a.dis:
        disassemble(a.pack, a.corpus, a.dis)
        return
    rows = collect(a.pack, a.corpus)
    if a.json:
        a.json.write_text(json.dumps(rows, indent=0))
    before = json.loads(a.before.read_text()) if a.before else None
    show(rows, before, a.top)


if __name__ == '__main__':
    main()
