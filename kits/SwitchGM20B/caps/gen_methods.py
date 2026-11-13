"""Every method GM20B's engine classes take, by name, and SwitchGM20BMethods.h, which host tests check pushes against.

NVIDIA's class headers give a method array's base and stride, not its length. GM20B's 3D class runs the Fermi MME, so
its shadow scratch words and macros are limits.tsv's mme_shadow_scratch and mme_macros (128 each), where Mesa's
class_parser.py generates Turing's 256; the compute class has 8 scratch words (class_parser.py's note, nvk_mme.h). Other
arrays take class_parser.py's lengths, and an array neither names runs on its stride up to the next method the header
defines. SwitchGM20BTegra.h adds the methods the headers lack.

usage: gen_methods.py --classes <dir> --class-parser <class_parser.py> --tegra <SwitchGM20BTegra.h>
                      --limits <limits.tsv> --out <SwitchGM20BMethods.h>
"""

import argparse
import ast
import fnmatch
import re
from pathlib import Path

CLASSES = {
    0xB197: ('clb197.h', 'NVB197_'),
    0xB1C0: ('clb1c0.h', 'NVB1C0_'),
    0xB0B5: ('clb0b5.h', 'NVB0B5_'),
    0x902D: ('cl902d.h', 'NV902D_'),
    0xA140: ('cla140.h', 'NVA140_'),
    0xB06F: ('clb06f.h', 'NVB06F_'),
}
TEGRA_PREFIXES = {'SWITCH_NVB197_': 0xB197, 'SWITCH_NVB06F_': 0xB06F}
COMPUTE_MME_SCRATCH = 8
METHODS_END = 0x4000
DEFINE = re.compile(r'^#define\s+(\w+?)(\(\w\))?\s+(.+?)\s*$', re.MULTILINE)
ADDRESS = re.compile(r'\(?\s*\(?(0x[0-9A-Fa-f]+)\)?\s*(?:\+\s*\(\w\)\s*\*\s*(\d+))?\s*\)?')


class ClassMethods:
    """One class's methods: fixed ones by address, arrays as (base, stride, length, name)."""

    def __init__(self) -> None:
        self.methods: dict[int, str] = {}
        self.arrays: list[tuple[int, int, int, str]] = []

    def name(self, method: int) -> str | None:
        if method in self.methods:
            return self.methods[method]
        for base, stride, length, name in self.arrays:
            if base <= method < base + stride * length and (method - base) % stride == 0:
                return f'{name}({(method - base) // stride})'
        return None

    def addresses(self) -> list[int]:
        out = set(self.methods)
        for base, stride, length, _ in self.arrays:
            out.update(range(base, base + stride * length, stride))
        return sorted(out)


def array_lengths(class_parser: Path) -> dict[str, int]:
    text = class_parser.read_text()
    start = text.index('METHOD_ARRAY_SIZES = {') + len('METHOD_ARRAY_SIZES = ')
    return ast.literal_eval(text[start : text.index('}', start) + 1])


def gm20b_lengths(limits: Path) -> dict[int, dict[str, int]]:
    rows = {}
    for line in limits.read_text().splitlines():
        if line and not line.startswith('#'):
            name, value, *_ = line.split('\t')
            rows[name] = value
    macros = int(rows['mme_macros'])
    return {
        0xB197: {
            'SET_MME_SHADOW_SCRATCH': int(rows['mme_shadow_scratch']),
            'CALL_MME_MACRO': macros,
            'CALL_MME_DATA': macros,
        },
        0xB1C0: {'SET_MME_SHADOW_SCRATCH': COMPUTE_MME_SCRATCH},
    }


def header_methods(text: str, prefix: str) -> tuple[dict[int, str], list[tuple[int, int, str]]]:
    """Read a header's fixed methods and its arrays as (base, stride, name).

    A field's defines (`hi:lo`) and the values under it follow their method; a define the method's name prefixes is a
    value of its own unless its address lies past the method's (SET_STENCIL_FUNC_REF after SET_STENCIL_FUNC, which
    class_parser.py takes for a value). The methods ascend, so what does not is none (a value, or clb06f.h's
    pushbuffer header encodings).
    """
    methods: dict[int, str] = {}
    arrays: list[tuple[int, int, str]] = []
    at, field = -1, None
    for match in DEFINE.finditer(text):
        full, index, value = match.groups()
        if not full.startswith(prefix):
            continue
        name = full[len(prefix) :]
        if ':' in value:
            field = name
            continue
        address = ADDRESS.fullmatch(value)
        if not address or (field and name.startswith(field + '_')):
            continue
        base = int(address.group(1), 16)
        if base <= at or base >= METHODS_END or base % 4:
            continue
        at, field = base, None
        if index:
            arrays.append((base, int(address.group(2) or 4), name))
        else:
            methods.setdefault(base, name)
    return methods, arrays


def unnamed_length(base: int, stride: int, methods: dict[int, str], bases: set[int]) -> int:
    end = min((m for m in methods if m > base), default=METHODS_END)
    length = 1
    while base + length * stride < end and base + length * stride not in bases:
        length += 1
    return length


def load(classes: Path, class_parser: Path, tegra: Path, limits: Path) -> dict[int, ClassMethods]:
    mesa = array_lengths(class_parser)
    gm20b = gm20b_lengths(limits)
    tegra_text = tegra.read_text()
    out = {}
    for cls, (header, prefix) in CLASSES.items():
        methods, arrays = header_methods((classes / header).read_text(), prefix)
        for tegra_prefix, owner in TEGRA_PREFIXES.items():
            if owner == cls:
                methods.update(header_methods(tegra_text, tegra_prefix)[0])
        bases = {base for base, _, _ in arrays}
        c = ClassMethods()
        c.methods = methods
        for base, stride, name in arrays:
            length = gm20b.get(cls, {}).get(name)
            if length is None:
                length = next((n for glob, n in mesa.items() if fnmatch.fnmatchcase(name, glob)), 0)
            if not length:
                length = unnamed_length(base, stride, methods, bases - {base})
            c.arrays.append((base, stride, length, name))
        out[cls] = c
    return out


def runs(addresses: list[int]) -> list[tuple[int, int]]:
    out: list[tuple[int, int]] = []
    for a in addresses:
        if out and out[-1][1] + 4 == a:
            out[-1] = (out[-1][0], a)
        else:
            out.append((a, a))
    return out


def header(classes: dict[int, ClassMethods]) -> str:
    out = [
        '// Generated by kits/SwitchGM20B/caps/gen_methods.py from the class headers and limits.tsv: do not edit.',
        '#pragma once',
        '',
        '#include <stdbool.h>',
        '#include <stddef.h>',
        '#include <stdint.h>',
        '',
        'struct switch_gm20b_method_run {',
        '    uint16_t first, last;',
        '};',
    ]
    for cls, c in classes.items():
        out += ['', f'static const struct switch_gm20b_method_run switch_gm20b_methods_{cls:04x}[] = {{']
        out += [f'    {{0x{a:04x}, 0x{b:04x}}},' for a, b in runs(c.addresses())]
        out.append('};')
    out += [
        '',
        'static inline bool switch_gm20b_method_in(const struct switch_gm20b_method_run *runs, size_t n, uint32_t m)',
        '{',
        '    for (size_t i = 0; i < n; i++)',
        '        if (m >= runs[i].first && m <= runs[i].last && (m & 3) == 0)',
        '            return true;',
        '    return false;',
        '}',
        '',
        '// Whether GM20B class `cls` (its number, 0xb197 and so on) takes method `mthd` (a byte address)',
        'static inline bool switch_gm20b_method_legal(uint32_t cls, uint32_t mthd)',
        '{',
        '    switch (cls) {',
    ]
    for cls in classes:
        table = f'switch_gm20b_methods_{cls:04x}'
        out += [
            f'    case 0x{cls:04x}:',
            f'        return switch_gm20b_method_in({table}, sizeof {table} / sizeof {table}[0], mthd);',
        ]
    out += ['    default:', '        return false;', '    }', '}', '']
    return '\n'.join(out)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--classes', type=Path, required=True)
    parser.add_argument('--class-parser', type=Path, required=True)
    parser.add_argument('--tegra', type=Path, required=True)
    parser.add_argument('--limits', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.write_text(header(load(args.classes, args.class_parser, args.tegra, args.limits)))


if __name__ == '__main__':
    main()
