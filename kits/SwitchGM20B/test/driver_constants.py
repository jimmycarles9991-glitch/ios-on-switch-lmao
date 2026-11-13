"""The drivers' hand-written GM20B constants, read out of their sources and checked against the class headers.

Every named constant a listed source defines (a `enum : uint32_t` entry, a namespace-scope `constexpr` integer or
array, a `#define` with a listed prefix) must have a row in driver_constants.tsv:

    file  name  nv|tegra|mismatch|policy  expression or reason  [note]

`nv` and `tegra` rows must equal their expression over kits/SwitchGM20B's headers; `mismatch` rows must differ from it
(a driver bug, named in the note: fixing it fails the test until the row says `nv`); `policy` rows are the driver's own
numbers. A constant that takes its value from the headers (class names and drf.h macros, and other such constants, no
number of its own) has no row: a row for one is stale. The output is a C++ header of static_asserts: a stale row, a
changed value or a new constant fails the build.
"""

import argparse
import re
import sys
from dataclasses import dataclass

DECLARATOR = re.compile(r'^\s*([A-Za-z_]\w*)\s*(\[[^\]]*\])?\s*=\s*(.*)$', re.DOTALL)
CONSTEXPR = re.compile(
    r'^constexpr\s+(?:uint32_t|uint64_t|uint8_t|uint16_t|int|unsigned)\s+(.*?);', re.MULTILINE | re.DOTALL
)
ENUM = re.compile(r'enum\s*:\s*uint32_t\s*\{(.*?)\};', re.DOTALL)
IDENT = re.compile(r'\b[A-Za-z_]\w*\b')
NUMBER = re.compile(r'\d')
CLASS_NAME = re.compile(r'^((SWITCH_)?NV[0-9A-F]{4}_\w+|NVDEF)$')
INDEXED = re.compile(r'\b((?:SWITCH_)?NV[0-9A-F]{4}_\w+)\s*\(\s*\d+\s*\)')
DRF = re.compile(r'\bNV(?:DEF|VAL)\s*\([^()]*\)')


@dataclass
class Constant:
    name: str
    array: bool
    value: str


def strip_comments(text: str) -> str:
    text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.DOTALL)
    return re.sub(r'//[^\n]*', '', text)


def split_top(text: str) -> list[str]:
    parts, depth, start = [], 0, 0
    for i, c in enumerate(text):
        if c in '([{':
            depth += 1
        elif c in ')]}':
            depth -= 1
        elif c == ',' and depth == 0:
            parts.append(text[start:i])
            start = i + 1
    parts.append(text[start:])
    return [p.strip() for p in parts if p.strip()]


def declarators(body: str) -> list[Constant]:
    out = []
    for part in split_top(body):
        m = DECLARATOR.match(part)
        if not m:
            continue
        name, array, value = m.groups()
        out.append(Constant(name, array is not None, ' '.join(value.split())))
    return out


def scan(text: str, define_prefix: str | None) -> list[Constant]:
    text = strip_comments(text)
    found = []
    for m in ENUM.finditer(text):
        found += declarators(m.group(1))
    for m in CONSTEXPR.finditer(text):
        body = m.group(1)
        if re.match(r'\s*\w+\s*\(', body):
            continue
        found += declarators(body)
    if define_prefix:
        for m in re.finditer(rf'^#define\s+({define_prefix}\w*)\s+(.+)$', text, re.MULTILINE):
            found.append(Constant(m.group(1), False, m.group(2).strip()))
    return found


def namespace_of(path: str) -> str:
    return re.sub(r'\W', '_', path.rsplit('/', 1)[-1])


def read_rows(path: str) -> dict[tuple[str, str], list[str]]:
    rows = {}
    with open(path, encoding='utf-8') as f:
        for n, line in enumerate(f, 1):
            if not line.strip() or line.startswith('#'):
                continue
            cols = line.rstrip('\n').split('\t')
            if len(cols) < 4 or cols[2] not in ('nv', 'tegra', 'mismatch', 'policy'):
                sys.exit(f'{path}:{n}: want file, name, nv|tegra|mismatch|policy, expression [, note]')
            if cols[2] == 'mismatch' and (len(cols) < 5 or not cols[4].strip()):
                sys.exit(f'{path}:{n}: a mismatch row names the bug in its note')
            rows[(cols[0], cols[1])] = cols
    return rows


def renamed(value: str, names: set[str]) -> str:
    def one(text: str) -> str:
        return re.sub(r'\b[A-Za-z_]\w*\b', lambda m: 'c_' + m.group(0) if m.group(0) in names else m.group(0), text)

    # A drf.h macro's arguments are class and field names, never the driver's own
    parts, at = [], 0
    for m in DRF.finditer(value):
        parts += [one(value[at : m.start()]), m.group(0)]
        at = m.end()
    return ''.join([*parts, one(value[at:])])


SAME = """namespace switch_gm20b_test {
template <unsigned long N> constexpr bool same(const uint64_t (&ours)[N], std::initializer_list<uint64_t> theirs)
{
    if (theirs.size() != N)
        return false;
    unsigned long i = 0;
    for (uint64_t v : theirs)
        if (ours[i++] != v)
            return false;
    return true;
}
}
"""


def from_headers(constants: list[Constant]) -> set[str]:
    """Return the constants that take their value from the class headers.

    Such a value is class names (an array method's by its index), drf.h macros over them and other such constants, with
    no number of its own.
    """
    found: set[str] = set()
    while True:
        more = set()
        for c in constants:
            value = DRF.sub('NVDEF', INDEXED.sub(r'\1', c.value))
            names = IDENT.findall(value)
            if c.name in found or not names or NUMBER.search(IDENT.sub('', value)):
                continue
            if all(CLASS_NAME.match(n) or n in found for n in names):
                more.add(c.name)
        if not more:
            return found
        found |= more


def checked_constants(short: str, constants: list[Constant], rows: dict, errors: list[str]) -> list:
    checked = []
    headers = from_headers(constants)
    for c in constants:
        row = rows.get((short, c.name))
        if c.name in headers:
            if row is not None:
                errors.append(f'{short}: {c.name} takes its value from the class headers: delete its row')
            checked.append((c, None))
        elif row is None:
            errors.append(f'{short}: {c.name} = {c.value} has no row in driver_constants.tsv')
        elif row[2] != 'policy':
            checked.append((c, row))
    return checked


def file_block(short: str, checked: list) -> list[str]:
    ns = namespace_of(short)
    names = {c.name for c, _ in checked}
    out = [f'namespace driver::{ns} {{']
    for c, _ in checked:
        value = renamed(c.value, names)
        out.append(
            f'constexpr uint64_t c_{c.name}[] = {value};' if c.array else f'constexpr uint64_t c_{c.name} = ({value});'
        )
    out.append('}')
    for c, row in checked:
        if row is None:
            continue
        ours, theirs = f'driver::{ns}::c_{c.name}', row[3]
        equal = f'switch_gm20b_test::same({ours}, {theirs})' if c.array else f'{ours} == uint64_t({theirs})'
        if row[2] == 'mismatch':
            out.append(f'static_assert(!({equal}), "{short} {c.name} now equals {theirs}: make its row nv");')
        else:
            out.append(f'static_assert({equal}, "{short} {c.name} != {theirs}");')
    return [*out, '']


def header(sources: dict[str, str], prefixes: dict[str, str], rows: dict) -> str:
    out = [
        '// Generated by kits/SwitchGM20B/test/driver_constants.py: do not edit.',
        '#pragma once',
        '#include "kits/SwitchGM20B/SwitchGM20B.h"',
        '#include <cstdint>',
        '#include <initializer_list>',
        '',
        SAME,
    ]
    seen, errors = set(), []
    for short, path in sources.items():
        with open(path, encoding='utf-8') as f:
            constants = scan(f.read(), prefixes.get(short))
        seen.update((short, c.name) for c in constants)
        out += file_block(short, checked_constants(short, constants, rows, errors))
    errors += [
        f'driver_constants.tsv: {f} no longer defines {n}' for f, n in rows if (f, n) not in seen and f in sources
    ]
    if errors:
        sys.exit('\n'.join(errors))
    return '\n'.join(out) + '\n'


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--rows', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--source', action='append', default=[], help='short=path[:define prefix]')
    args = parser.parse_args()
    sources, prefixes = {}, {}
    for spec in args.source:
        short, path = spec.split('=', 1)
        if ':' in path:
            path, prefixes[short] = path.split(':', 1)
        sources[short] = path
    text = header(sources, prefixes, read_rows(args.rows))
    with open(args.out, 'w', encoding='utf-8') as f:
        f.write(text)


if __name__ == '__main__':
    main()
