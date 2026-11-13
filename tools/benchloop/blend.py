"""The ROP's iterated blend unit, run from a microcode table and checked against the blend the table means.

A table (`iterated-blend/1`, JSON) holds one blend's microcode and what it should compute:

    {"format": "iterated-blend/1", "name": "multiply", "passes": 1, "alpha": false,
     "instructions": [{"operation": "ADD_PRODUCTS", "operand_a": "SRC_RGB", "operand_b": "DEST_RGB",
                       "operand_c": "SRC_RGB", "operand_d": "ONE_MINUS_DEST_AAA", "pass_output": "TEMP0"},
                      "0x302a0000"],
     "constants": [[0, 0, 0]],
     "methods": [[4960, 1]],
     "rgb": "src.rgb * dst.rgb + src.rgb * (1 - dst.a) + dst.rgb * (1 - src.a)",
     "a": "src.a + dst.a - src.a * dst.a",
     "tolerance": 1}

An instruction is a LOAD_ITERATED_BLEND_INSTRUCTION word, given whole (a hex string) or by the fields clb197.h
names, each value one of that field's names there (or a number). `passes` is SET_ITERATED_BLEND_PASS and `alpha`
SET_ITERATED_BLEND's ALPHA_ENABLE, which takes alpha from an R_TO_A pass; without it alpha comes from the common blend
block, set to source over here (the unit refuses per-target blend state and runs only under SET_BLEND(0)).
`constants` are SET_ITERATED_BLEND_CONSTANT_RED/GREEN/BLUE(j) words; `methods` are other state the blend needs, set
with it, from protocol.METHOD_RANGES, and `restore` (optional) what each of them is set back to after the draw, 0 for
one it does not name. The per-target blend switch goes back to 1, as the context has it, and the pass count is never
set back (0 is INVALID_VALUE). `rgb` and `a` are formula.py expressions; `tolerance` is in codes of the target. Each
experiment draws one source colour per column
over one destination colour per row and reads every texel back.
"""

import json
import random
import struct
from pathlib import Path

from tools.benchloop import columns, formula, protocol, texels

TABLE_FORMAT = 'iterated-blend/1'
SET_ITERATED_BLEND = 0x0DD0
SET_ITERATED_BLEND_PASS = 0x0DD4
LOAD_POINTER = 0x0DF0
LOAD_INSTRUCTION = 0x0DF4
CONSTANT_RED = 0x1480

# clb197.h NVB197_LOAD_ITERATED_BLEND_INSTRUCTION_*: (low bit, high bit, value names)
FIELDS = {
    'test_cc': (0, 2, ['FALSE', 'TRUE', 'EQ', 'NE', 'LT', 'LE', 'GT', 'GE']),
    'operation': (3, 5, ['ADD_PRODUCTS', 'SUB_PRODUCTS', 'MIN', 'MAX', 'RCP', 'ADD', 'SUBTRACT']),
    'constant_input_select': (6, 8, [f'CONSTANT{i}' for i in range(8)]),
    'operand_a': (
        9,
        11,
        ['SRC_RGB', 'DEST_RGB', 'SRC_AAA', 'DEST_AAA', 'TEMP0_RGB', 'TEMP1_RGB', 'TEMP2_RGB', 'PBR_RGB'],
    ),
    'operand_b': (
        12,
        15,
        [
            'ZERO',
            'ONE',
            'SRC_RGB',
            'SRC_AAA',
            'ONE_MINUS_SRC_AAA',
            'DEST_RGB',
            'DEST_AAA',
            'ONE_MINUS_DEST_AAA',
            None,
            'TEMP0_RGB',
            'TEMP1_RGB',
            'TEMP2_RGB',
            'PBR_RGB',
            'CONSTANT_RGB',
            'ZERO_A_TIMES_B',
        ],
    ),
    'operand_c': (
        16,
        18,
        ['SRC_RGB', 'DEST_RGB', 'SRC_AAA', 'DEST_AAA', 'TEMP0_RGB', 'TEMP1_RGB', 'TEMP2_RGB', 'PBR_RGB'],
    ),
    'operand_d': (
        19,
        22,
        [
            'ZERO',
            'ONE',
            'SRC_RGB',
            'SRC_AAA',
            'ONE_MINUS_SRC_AAA',
            'DEST_RGB',
            'DEST_AAA',
            'ONE_MINUS_DEST_AAA',
            None,
            'TEMP0_RGB',
            'TEMP1_RGB',
            'TEMP2_RGB',
            'PBR_RGB',
            'CONSTANT_RGB',
            'ZERO_C_TIMES_D',
        ],
    ),
    'output_swizzle': (23, 25, ['RGB', 'GBR', 'RRR', 'GGG', 'BBB', 'R_TO_A']),
    'output_write_mask': (26, 27, ['RGB', 'R_ONLY', 'G_ONLY', 'B_ONLY']),
    'pass_output': (28, 29, ['TEMP0', 'TEMP1', 'TEMP2', 'NONE']),
    'set_cc': (31, 31, ['FALSE', 'TRUE']),
}


class TableError(ValueError):
    pass


def field_values(word: int) -> dict[str, str | int]:
    out: dict[str, str | int] = {}
    for name, (lo, hi, names) in FIELDS.items():
        v = word >> lo & ((1 << (hi - lo + 1)) - 1)
        out[name] = names[v] if v < len(names) and names[v] else v
    return out


def instruction(spec: dict | str | int) -> int:
    if isinstance(spec, int):
        return spec
    if isinstance(spec, str):
        return int(spec, 16)
    word = 0
    for name, value in spec.items():
        if name not in FIELDS:
            raise TableError(f'instruction field {name!r} is not one of {sorted(FIELDS)}')
        lo, hi, names = FIELDS[name]
        v = value if isinstance(value, int) else names.index(value) if value in names else None
        if v is None or not 0 <= v < 1 << (hi - lo + 1):
            raise TableError(f'{name}={value!r}: one of {[n for n in names if n]}')
        word |= v << lo
    return word


class Table:
    def __init__(self, fields: dict) -> None:
        if fields.get('format') != TABLE_FORMAT:
            raise TableError(f'not an {TABLE_FORMAT} table')
        self.name = fields['name']
        self.passes = int(fields.get('passes', 1))
        self.alpha = bool(fields.get('alpha', False))
        self.words = [instruction(i) for i in fields['instructions']]
        if self.alpha != any(field_values(w)['output_swizzle'] == 'R_TO_A' for w in self.words):
            raise TableError(f'{self.name}: alpha (ALPHA_ENABLE) goes with an R_TO_A pass, and only with one')
        self.constants = [[int(c) & 0xFFFFFFFF for c in rgb] for rgb in fields.get('constants', [])]
        self.extra = [list(p) for p in fields.get('methods', [])]
        self.restore = {int(m): int(v) for m, v in fields.get('restore', [])}
        self.rgb = formula.Formula(fields['rgb'])
        self.a = formula.Formula(fields['a'])
        self.tolerance = int(fields.get('tolerance', 1))
        if not self.words or len(self.constants) > 8 or not 1 <= self.passes <= 255:
            raise TableError(f'{self.name}: 1..256 instructions, at most 8 constants, 1..255 passes')

    @classmethod
    def load(cls, path: Path) -> 'Table':
        return cls(json.loads(path.read_text()))

    def constant_colours(self) -> list[formula.Colour]:
        return [(*(struct.unpack('<f', struct.pack('<I', w))[0] for w in rgb), 1.0) for rgb in self.constants]

    def methods(self, blend_enable: bool | None) -> tuple[list[list[int]], list[list[int]]]:
        """Give the methods set with the draw, and the ones that set the state back after it."""
        common = [[0x12E4, 0], [0x133C, 1], [0x1340, 0x8006], [0x1344, 0x4001], [0x1348, 0x4000]]
        common += [[0x134C, 0x8006], [0x1350, 0x4001], [0x1358, 0x4303]]
        set_up = [*common, [SET_ITERATED_BLEND_PASS, self.passes], [LOAD_POINTER, 0]]
        set_up += [[LOAD_INSTRUCTION, w] for w in self.words]
        for j, (r, g, b) in enumerate(self.constants):
            set_up += [[CONSTANT_RED + 16 * j, r], [CONSTANT_RED + 16 * j + 4, g], [CONSTANT_RED + 16 * j + 8, b]]
        set_up += self.extra
        restore = [[SET_ITERATED_BLEND, 0]] + [[m, self.restore.get(m, 0)] for m, _ in self.extra] + [[0x12E4, 1]]
        set_up.append([0x1360, int(blend_enable is not False)])  # SET_BLEND(0): the unit runs only under it
        restore.append([0x1360, 0])
        set_up.append([SET_ITERATED_BLEND, 1 | int(self.alpha) << 1])
        return set_up, restore


def colours(n: int, seed: int) -> list[formula.Colour]:
    """Give the corners of the colour cube with alpha 0, 1/2 and 1 first, then seeded random colours."""
    corners = [(r, g, b, a) for a in (0.0, 0.5, 1.0) for r in (0.0, 1.0) for g in (0.0, 1.0) for b in (0.0, 1.0)]
    rng = random.Random(seed)
    rest = [tuple(rng.randrange(256) / 255 for _ in range(4)) for _ in range(max(0, n - len(corners)))]
    return (corners + rest)[:n]


def generate(table: Table, fmt: str = 'bgra8unorm', side: int = 32, blend_enables: tuple = (None,)) -> list[dict]:
    # the expectation takes the inputs as the device holds them: sources as f32, destinations as the target's codes
    src = [tuple(texels.f32(v) for v in c) for c in colours(side, 1)]
    dst = [texels.decode(texels.encode(c, fmt), fmt) for c in colours(side, 2)]
    fill = b''.join(texels.encode(dst[y], fmt) for y in range(side) for _ in range(side))
    out = []
    for enable in blend_enables:
        methods, restore = table.methods(enable)
        suffix = {None: '', True: '-blend', False: '-noblend'}[enable]
        out.append(
            {
                'id': f'blend-{table.name}{suffix}',
                'kind': 'readback',
                'target': {'format': fmt, 'width': side, 'height': side},
                'fill': protocol.b64(fill),
                'program': columns.program(src),
                'methods': methods,
                'restore': restore,
                'readback': 'full',
                'meta': {'family': 'blend', 'table': table.name, 'blend_enable': enable, 'src': src, 'dst': dst},
            }
        )
    return out


def check(table: Table, experiment: dict, result: dict) -> dict:
    """Every texel against the table's blend of its column's source over its row's destination."""
    fmt = experiment['target']['format']
    side = experiment['target']['width']
    src, dst = experiment['meta']['src'], experiment['meta']['dst']
    row = {'id': experiment['id'], 'status': result.get('status'), 'checked': 0, 'exact': 0, 'within': 0}
    data = (result.get('texels') or {}).get('data')
    if result.get('status') != 'ok' or not data:
        return row | {'why': result.get('why', 'no texels came back')}
    worst, histogram = None, {}
    for index, texel in enumerate(texels.split(protocol.unb64(data), fmt)):
        x, y = index % side, index // side
        want = texels.encode(formula.blend(table.rgb, table.a, src[x], dst[y], table.constant_colours()), fmt)
        got, expected = texels.codes(texel, fmt), texels.codes(want, fmt)
        error = max(abs(a - b) for a, b in zip(got, expected, strict=True))
        histogram[error] = histogram.get(error, 0) + 1
        row['checked'] += 1
        row['exact'] += error == 0
        row['within'] += error <= table.tolerance
        if worst is None or error > worst[0]:
            worst = (error, x, y, got, expected)
    row['errors'] = dict(sorted(histogram.items()))
    if worst:
        row['worst'] = {'codes_off': worst[0], 'at': [worst[1], worst[2]], 'got': worst[3], 'want': worst[4]}
    return row
