#!/usr/bin/env python3
"""The channel's context init (context.tsv, macros.txt) as gen/SwitchGM20BContext.h: one method per word, in order.

usage: gen_context.py --table context.tsv --macros macros.txt --classes <dir of clb197.h, clb1c0.h> --out <header>

A row's method is a class header name, `NAME(i)` or `NAME(i..j)` for one write per index, or `macro`, whose value
names the next MME start slot's program in macros.txt. Its value is one word, `v*n` for n writes of v, or `a..b/s`
for a, a+s, ..., b (per index of a range). A relocated row's value is 0: the method takes the address the channel
opener supplies. Every row gives its reason.
"""

import argparse
import re
import sys
from collections.abc import Iterator

CLASSES = {'b197': ('clb197.h', 'NVB197_', 0), 'b1c0': ('clb1c0.h', 'NVB1C0_', 1)}
RELOCS = [
    '-',
    'heap_hi',
    'heap_lo',
    'zero_hi',
    'zero_lo',
    'root_hi',
    'root_lo',
    'tic_hi',
    'tic_lo',
    'tic_max',
    'tsc_hi',
    'tsc_lo',
    'tsc_max',
    'tickets_lo',
    'tickets_hi',
]
MME_START_POINTER, MME_START, MME_RAM_POINTER, MME_RAM = 0x11C, 0x120, 0x114, 0x118


class TableError(Exception):
    pass


def methods(path: str, prefix: str) -> tuple[dict[str, int], dict[str, tuple[int, int]]]:
    with open(path, encoding='utf-8') as f:
        text = f.read()
    scalars = {
        m.group(1): int(m.group(2), 16)
        for m in re.finditer(rf'^#define {prefix}(\w+)\s+(0x[0-9A-Fa-f]+)\s*$', text, re.MULTILINE)
    }
    arrays = {
        m.group(1): (int(m.group(3), 16), int(m.group(4)))
        for m in re.finditer(
            rf'^#define {prefix}(\w+)\((\w)\)\s+\((0x[0-9A-Fa-f]+)\+\(\2\)\*(\d+)\)', text, re.MULTILINE
        )
    }
    return scalars, arrays


def values(text: str) -> list[int]:
    if m := re.fullmatch(r'(0x[0-9a-f]+)\*(\d+)', text):
        return [int(m.group(1), 16)] * int(m.group(2))
    if m := re.fullmatch(r'(0x[0-9a-f]+)\.\.(0x[0-9a-f]+)/(0x[0-9a-f]+)', text):
        first, last, step = (int(g, 16) for g in m.groups())
        return list(range(first, last + 1, step))
    if re.fullmatch(r'0x[0-9a-f]+', text):
        return [int(text, 16)]
    raise TableError(f'value {text!r}')


def addresses(method: str, scalars: dict, arrays: dict) -> list[int]:
    if m := re.fullmatch(r'(\w+)\((\d+)(?:\.\.(\d+))?\)', method):
        name, first = m.group(1), int(m.group(2))
        last = int(m.group(3)) if m.group(3) else first
        if name not in arrays or last < first:
            raise TableError(f'{method!r} is no method array of the class')
        base, stride = arrays[name]
        return [base + i * stride for i in range(first, last + 1)]
    if method not in scalars:
        raise TableError(f'{method!r} is no method of the class')
    return [scalars[method]]


def read_macros(path: str) -> list[tuple[str, list[int]]]:
    macros = []
    with open(path, encoding='utf-8') as f:
        for line in f:
            if line.strip() and not line.startswith('#'):
                name, *words = line.split()
                macros.append((name, [int(w, 16) for w in words]))
    return macros


def rows(table: str) -> Iterator[tuple[int, list[str]]]:
    with open(table, encoding='utf-8') as f:
        lines = f.read().split('\n')
    for n, line in enumerate(lines, 1):
        if not line or line.startswith('#'):
            continue
        cols = line.split('\t')
        if len(cols) != 5 or not cols[4].strip():
            raise TableError(f'context.tsv:{n}: class, method, value, reloc and a reason')
        if cols[0] not in CLASSES or cols[3] not in RELOCS:
            raise TableError(f'context.tsv:{n}: class {cols[0]!r} or reloc {cols[3]!r}')
        yield n, cols[:4]


def upload(subc: int, slot: int, ram: int, words: list[int]) -> list[tuple[int, int, int, int]]:
    out = [(subc, MME_START_POINTER, slot, 0), (subc, MME_START, ram, 0), (subc, MME_RAM_POINTER, ram, 0)]
    return out + [(subc, MME_RAM, w, 0) for w in words]


def expand(table: str, macro_path: str, classes: str) -> list[tuple[int, int, int, int]]:
    macros = read_macros(macro_path)
    headers = {cls: methods(f'{classes}/{h}', p) for cls, (h, p, _) in CLASSES.items()}
    out, slot, ram = [], 0, 0
    for n, (cls, method, value, reloc) in rows(table):
        subc = CLASSES[cls][2]
        if method == 'macro':
            if slot >= len(macros) or macros[slot][0] != value:
                raise TableError(f'context.tsv:{n}: macro {value!r} is not slot {slot} of macros.txt')
            out += upload(subc, slot, ram, macros[slot][1])
            slot, ram = slot + 1, ram + len(macros[slot][1])
            continue
        try:
            where, data = addresses(method, *headers[cls]), values(value)
        except TableError as e:
            raise TableError(f'context.tsv:{n}: {e}') from None
        if reloc != '-' and data != [0]:
            raise TableError(f'context.tsv:{n}: a relocated method takes 0')
        out += [(subc, a, d, RELOCS.index(reloc)) for a in where for d in data]
    if slot != len(macros):
        raise TableError(f'macros.txt: {len(macros) - slot} programs no row uploads')
    return out


def header(stream: list[tuple[int, int, int, int]]) -> str:
    out = [
        '// Generated by kits/SwitchGM20BEmit/context/gen_context.py from context.tsv and macros.txt.',
        '#pragma once',
        '#include <stdint.h>',
        '',
        'enum switch_gm20b_context_reloc {',
    ]
    out += [f'    SWITCH_GM20B_CONTEXT_{r.upper() if r != "-" else "FIXED"},' for r in RELOCS]
    out += [
        '};',
        '',
        'struct switch_gm20b_context_method {',
        '    uint16_t subc, mthd;',
        '    uint32_t data, reloc;',
        '};',
        '',
        f'#define SWITCH_GM20B_CONTEXT_METHODS {len(stream)}u',
        '',
        'static const struct switch_gm20b_context_method switch_gm20b_context[SWITCH_GM20B_CONTEXT_METHODS] = {',
    ]
    out += [f'    {{{s}, 0x{m:04x}, 0x{d:x}, {r}}},' for s, m, d, r in stream]
    out += ['};', '']
    return '\n'.join(out)


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument('--table', required=True)
    p.add_argument('--macros', required=True)
    p.add_argument('--classes', required=True)
    p.add_argument('--out', required=True)
    args = p.parse_args()
    try:
        stream = expand(args.table, args.macros, args.classes)
    except TableError as e:
        sys.exit(f'gen_context: {e}')
    with open(args.out, 'w', encoding='utf-8') as f:
        f.write(header(stream))


if __name__ == '__main__':
    main()
