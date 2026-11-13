"""The GM20B capability table: formats.tsv, limits.tsv, features.tsv and methods.tsv, validated and joined.

A row's mechanism names methods or fields of the class headers (`b197 SET_ANTI_ALIAS, ...; tegra SWITCH_...`), or is
`hw: <text>` for what no method controls. A citation is `path[:line]` inside the repository or `ext: <text>`; that a
cited file and line exist is checked only given the repository root (`bazel run :caps_report`), since a build sees its
declared inputs, not the tree. A HW Test id is one hw_test_app's sources define, or `planned:<id>`. Format rows name the
Mesa pipe format whose NIL row (nil_formats.csv, read through NIL's own generator) gives the GM20B colour/zeta target,
texture header and support bits.
"""

import csv
import os
import re
import sys
from dataclasses import dataclass, field

GRADES = ('source', 'host', 'chip')
CLASSES = {
    'b197': ('clb197.h', 'NVB197_'),
    'b197tex': ('clb197tex.h', 'NVB197_'),
    'b1c0': ('clb1c0.h', 'NVB1C0_'),
    'b1c0qmd': ('clb1c0qmd.h', 'NVB1C0_QMDV01_07_'),
    'b0b5': ('clb0b5.h', 'NVB0B5_'),
    '902d': ('cl902d.h', 'NV902D_'),
    'a140': ('cla140.h', 'NVA140_'),
    'b06f': ('clb06f.h', 'NVB06F_'),
    '906f': ('cl906f.h', 'NV906F_'),
}
# Extra headers whose names NIL's generator emits (Fermi/Maxwell A spellings of the same enumerations).
VALUE_HEADERS = ['cl9097.h', 'cl9097tex.h', 'clb097.h', 'clb097tex.h', 'clb197.h', 'clb197tex.h', 'cl902d.h']
DEFINE = re.compile(r'^#define\s+(\w+)\s+(.+?)\s*$', re.MULTILINE)


class CapsError(Exception):
    pass


@dataclass
class Row:
    table: str
    line: int
    cols: dict

    def where(self) -> str:
        return f'{self.table}.tsv:{self.line}'


@dataclass
class Caps:
    formats: list = field(default_factory=list)
    limits: list = field(default_factory=list)
    features: list = field(default_factory=list)
    methods: list = field(default_factory=list)


def read_tsv(path: str, table: str) -> list[Row]:
    with open(path, encoding='utf-8') as f:
        lines = f.read().split('\n')
    header = lines[0].lstrip('# ').split('\t')
    rows = []
    for n, line in enumerate(lines[1:], 2):
        if not line or line.startswith('#'):
            continue
        cols = line.split('\t')
        if len(cols) != len(header):
            raise CapsError(f'{table}.tsv:{n}: {len(cols)} columns, the header has {len(header)}')
        rows.append(Row(table, n, dict(zip(header, cols, strict=True))))
    return rows


def header_defines(classes_dir: str, name: str) -> dict[str, str]:
    with open(os.path.join(classes_dir, name), encoding='utf-8') as f:
        return {m.group(1): m.group(2) for m in DEFINE.finditer(f.read())}


def int_value(text: str) -> int | None:
    text = text.strip().strip('()')
    if re.fullmatch(r'0x[0-9A-Fa-f]+|\d+', text):
        return int(text, 0)
    return None


def field_bits(text: str) -> tuple[int, int] | None:
    m = re.fullmatch(r'(?:MW\()?\s*(\d+):(\d+)\s*\)?', text.strip())
    return (int(m.group(1)), int(m.group(2))) if m else None


class Headers:
    def __init__(self, classes_dir: str, tegra_header: str):
        self.by_class = {key: header_defines(classes_dir, f) for key, (f, _) in CLASSES.items()}
        with open(tegra_header, encoding='utf-8') as f:
            self.tegra = {m.group(1): m.group(2) for m in DEFINE.finditer(f.read())}
        self.values = {}
        for name in VALUE_HEADERS:
            self.values.update(header_defines(classes_dir, name))

    def resolves(self, cls: str, name: str) -> bool:
        if cls == 'tegra':
            return name in self.tegra
        defines = self.by_class[cls]
        full = CLASSES[cls][1] + name
        return full in defines or any(d.startswith(full + '_') for d in defines)

    def value(self, name: str) -> int:
        text = self.values.get(name)
        v = int_value(text) if text is not None else None
        if v is None:
            raise CapsError(f'{name} is not a numeric define of the class headers')
        return v

    def field(self, name: str) -> tuple[int, int]:
        bits = field_bits(self.values.get(name, ''))
        if bits is None:
            raise CapsError(f'{name} is not a field of the class headers')
        return bits


def check_mechanism(row: Row, headers: Headers) -> list[tuple[str, str]]:
    text = row.cols['mechanism'].strip()
    if text.startswith('hw:'):
        if len(text) < 6:
            raise CapsError(f'{row.where()}: an hw: mechanism says what does it')
        return []
    names = []
    for part in text.split(';'):
        cls, _, rest = part.strip().partition(' ')
        if cls not in CLASSES and cls != 'tegra':
            raise CapsError(f'{row.where()}: unknown class {cls!r} in the mechanism')
        for name in (n.strip() for n in rest.split(',')):
            if not name or not headers.resolves(cls, name):
                raise CapsError(f'{row.where()}: {cls} {name!r} is in no class header')
            names.append((cls, name))
    return names


def check_citation(row: Row, root: str | None) -> None:
    refs = [r.strip() for r in row.cols['citation'].split(';') if r.strip()]
    if not refs:
        raise CapsError(f'{row.where()}: no citation')
    for ref in refs:
        if ref.startswith('ext:'):
            if len(ref) < 8:
                raise CapsError(f'{row.where()}: an ext: citation names its source')
            continue
        path, _, line = ref.partition(':')
        if not re.fullmatch(r'[\w./+-]+', path) or (line and not line.isdigit()):
            raise CapsError(f'{row.where()}: citation {ref!r} is neither path[:line] nor ext: <source>')
        if root is None:
            continue
        full = os.path.join(root, path)
        if not os.path.isfile(full):
            raise CapsError(f'{row.where()}: cited {path} is not in the repository')
        if line:
            with open(full, encoding='utf-8', errors='replace') as f:
                count = sum(1 for _ in f)
            if not line.isdigit() or int(line) > count:
                raise CapsError(f'{row.where()}: {path} has no line {line}')


def check_row(row: Row, headers: Headers, root: str | None, hwtests: set[str]) -> None:
    grade = row.cols['grade']
    if grade not in GRADES:
        raise CapsError(f'{row.where()}: grade {grade!r} is not one of {", ".join(GRADES)}')
    check_citation(row, root)
    test = row.cols['hwtest']
    if test.startswith('planned:'):
        if not re.fullmatch(r'planned:[a-z0-9_.]+', test):
            raise CapsError(f'{row.where()}: planned HW Test id {test!r}')
    elif test not in hwtests:
        raise CapsError(f'{row.where()}: HW Test {test!r} is not in hw_test_app (planned:<id> if it is to come)')
    if 'mechanism' in row.cols:
        check_mechanism(row, headers)


def hw_test_ids(sources: list[str]) -> set[str]:
    ids = set()
    for path in sources:
        with open(path, encoding='utf-8') as f:
            ids.update(re.findall(r'"([a-z0-9]+\.[a-z0-9_.]+)"', f.read()))
    return ids


def load(tables_dir: str, root: str | None, headers: Headers, hwtests: set[str]) -> Caps:
    caps = Caps()
    for table in ('formats', 'limits', 'features'):
        rows = read_tsv(os.path.join(tables_dir, table + '.tsv'), table)
        keys = set()
        for row in rows:
            check_row(row, headers, root, hwtests)
            key = row.cols.get('mtl') or row.cols.get('name') or row.cols.get('id')
            if key in keys:
                raise CapsError(f'{row.where()}: {key} twice')
            keys.add(key)
        setattr(caps, table, rows)
    ids = {r.cols['id'] for r in caps.features}
    for row in read_tsv(os.path.join(tables_dir, 'methods.tsv'), 'methods'):
        cls, method = row.cols['class'], row.cols['method']
        if not headers.resolves(cls, method):
            raise CapsError(f'{row.where()}: {cls} {method} is in no class header')
        for rid in row.cols['feature row(s) in features.tsv'].split('+'):
            if rid not in ids and not rid.endswith('-housekeeping'):
                raise CapsError(f'{row.where()}: feature row {rid} does not exist')
        caps.methods.append(row)
    return caps


def fail(error: CapsError) -> None:
    sys.exit(f'kits/SwitchGM20B/caps: {error}')


def read_nil(csv_path: str) -> dict[str, list[str]]:
    rows = {}
    with open(csv_path, encoding='utf-8') as f:
        for line in csv.reader(f):
            if line and not line[0].startswith('#') and line[0].strip():
                rows[line[0].strip()] = line
    return rows
