"""Falcon firmware reader: the image's app table, disassembly, and the data/I/O accesses an app makes.

bazel run //tools/falcon -- apps NVENC.BIN
bazel run //tools/falcon -- disasm NVENC.BIN 0x1f00 0x2000
bazel run //tools/falcon -- accesses NVENC.BIN --entry 0x1f00 --region 0xa00:0x400 [--kinds ld,st]
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from tools.falcon import image, isa
from tools.falcon.flow import Program


def _int(text: str) -> int:
    return int(text, 0)


def apps(img: image.Image) -> list[str]:
    lines = [
        f'os code {img.os_code.offset:#x}+{img.os_code.size:#x} data {img.os_data.offset:#x}+{img.os_data.size:#x}'
    ]
    lines.append(f'os overlay {img.overlay.offset:#x}+{img.overlay.size:#x}')
    lines += [f'app {i} code {c.offset:#x}+{c.size:#x}' for i, c in enumerate(img.app_code) if c.size]
    lines += [f'data {i} {d.offset:#x}+{d.size:#x}' for i, d in enumerate(img.app_data) if d.size]
    return lines


def disasm(code: bytes, start: int, end: int) -> list[str]:
    lines, pc = [], start
    while pc < end:
        insn = isa.decode(code, pc, pc)
        if insn is None:
            lines.append(f'{pc:#07x}: .byte {code[pc]:#04x}')
            pc += 1
            continue
        lines.append(f'{pc:#07x}: {code[pc : pc + insn.size].hex():12s} {insn}')
        pc += insn.size
    return lines


def accesses(code: bytes, entries: list[int], base: int, size: int, kinds: set[str]) -> list[str]:
    program = Program(code, entries)
    hits = [
        a for a in program.accesses if a.kind in kinds and a.address is not None and base <= a.address < base + size
    ]
    hits.sort(key=lambda a: (a.address, a.width, a.code))
    return [
        f'{a.kind:5s} +{a.address - base:#05x} b{a.width:<3d} @{a.code:#07x} fn {a.function:#07x} {a.value}'
        for a in hits
        if a.address is not None
    ]


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('apps').add_argument('image', type=Path)
    d = sub.add_parser('disasm')
    d.add_argument('image', type=Path)
    d.add_argument('start', type=_int)
    d.add_argument('end', type=_int)
    a = sub.add_parser('accesses')
    a.add_argument('image', type=Path)
    a.add_argument('--entry', type=_int, action='append', required=True)
    a.add_argument('--region', required=True, help='BASE:SIZE in data memory')
    a.add_argument('--kinds', default='ld,st,xdld,xdst')
    args = parser.parse_args(argv)
    img = image.parse(args.image.read_bytes())
    if args.command == 'apps':
        lines = apps(img)
    elif args.command == 'disasm':
        lines = disasm(img.ucode, args.start, args.end)
    else:
        base, size = (_int(x) for x in args.region.split(':'))
        lines = accesses(img.ucode, args.entry, base, size, set(args.kinds.split(',')))
    print('\n'.join(lines))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
