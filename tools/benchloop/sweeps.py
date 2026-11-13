"""The ROP option sweep and the one-target iterated blend sweep.

precision: the store sweep (rop.py) and a fixed-function blend sweep (five factor pairs over 64 random f32 sources and
64 random destinations), each once as the context has it and once per ROP option set to its other value, the option
restored to NVK's context-init value after the draw. analyse_precision says which options change a texel and whether any
setting makes the store or the blend match Metal's rounding.

iterated: the iterated blend unit with colour 0 bound alone (the unit takes one target), a single-output pixel header,
SET_BLEND_STATE_PER_TARGET false: the setup variants first, then each program of a rung-3 microcode table
(out/agent-scratch/x1-f1/rung3/microcode.tsv: name, tags, method, value rows), its constants renumbered so the first
pass reads constant 0 (ibprogram.py). A primer draws each experiment's destination rows, blend off, just before it: on
12 Nov about one blend in twenty read the previous experiment's texels instead of its own CPU fill
(x1-bench3/BENCH3.md), and with the primer the stale texels are the fill's own.
"""

import csv
import math
import random
import struct
from fractions import Fraction
from pathlib import Path

from tools.benchloop import blend, columns, ibprogram, protocol, rop, texels

SIDE = 64
FUNC_ADD = 0x8006
FACTOR = {
    'zero': 0x4000,
    'one': 0x4001,
    'src_color': 0x4300,
    'src_alpha': 0x4302,
    'one_minus_src_alpha': 0x4303,
    'dst_alpha': 0x4304,
    'dst_color': 0x4306,
}
# colour source, colour destination, alpha source, alpha destination
MODES = {
    'sov-pre': ('one', 'one_minus_src_alpha', 'one', 'one_minus_src_alpha'),
    'sov-straight': ('src_alpha', 'one_minus_src_alpha', 'one', 'one_minus_src_alpha'),
    'add': ('one', 'one', 'one', 'one'),
    'mul-dst': ('dst_color', 'zero', 'dst_alpha', 'zero'),
    'mul-src': ('zero', 'src_color', 'zero', 'src_alpha'),
}
NVK_THRESHOLDS = [[0x10CC, 0x4], [0x10E0, 0xFF], [0x10E4, 0xFF], [0x10E8, 0x3F], [0x10EC, 0xFF], [0x10F0, 0x4]]
# name: (set with the draw, restore = NVK's context-init value, nvk_cmd_draw.c nvk_push_draw_state_init)
OPTIONS = {
    'base': ([], []),
    'reduce-on': ([[0x0D9C, 1]], [[0x0D9C, 0]]),
    'reduce-on-thresholds-0': ([[0x0D9C, 1]] + [[m, 0] for m, _ in NVK_THRESHOLDS], [[0x0D9C, 0], *NVK_THRESHOLDS]),
    'blend-float-option-0': ([[0x19C0, 0]], [[0x19C0, 1]]),
    'color-clamp-off': ([[0x2600, 0]], [[0x2600, 1]]),
    'blend-per-format-off': ([[0x1140, 0]], [[0x1140, 1 << 4]]),
    'ps-saturate-0': ([[0x13A8, 1]], [[0x13A8, 0]]),
    'blend-opt-control-0': ([[0x0FDC, 0]], [[0x0FDC, 1]]),
    'single-rop-1': ([[0x135C, 1]], [[0x135C, 0]]),
    'single-ct-write-0': ([[0x0F90, 0]], [[0x0F90, 1]]),
    'ct-mrt-enable-0': ([[0x0FAC, 0]], [[0x0FAC, 1]]),
}
SRGB_OPTIONS = {'srgb-write-1': ([[0x15B8, 1]], [[0x15B8, 0]]), 'srgb-write-0': ([[0x15B8, 0]], [[0x15B8, 0]])}
STORE_FORMATS = ('bgra8unorm', 'rgb10a2unorm', 'rgba16float')
BLEND_FORMATS = ('bgra8unorm', 'rgba16float')


def blend_state(mode: str) -> tuple[list[list[int]], list[list[int]]]:
    cs, cd, as_, ad = (FACTOR[x] for x in MODES[mode])
    words = [1, FUNC_ADD, cs, cd, FUNC_ADD, as_, ad]
    return [[0x12E4, 1], *([0x1E00 + 4 * k, v] for k, v in enumerate(words)), [0x1360, 1]], [[0x1360, 0]]


def _sources(rng: random.Random) -> list[tuple[float, ...]]:
    fixed = [(0.0, 0.0, 0.0, 0.0), (1.0, 1.0, 1.0, 1.0), (0.5, 0.5, 0.5, 0.5), (0.25, 0.75, 0.5, 0.5)]
    return fixed + [tuple(texels.f32(rng.random()) for _ in range(4)) for _ in range(SIDE - len(fixed))]


def _destination(rng: random.Random, fmt: str) -> bytes:
    if fmt == 'rgba16float':
        return struct.pack('<4e', *(rng.randrange(0, 1025) / 1024 for _ in range(4)))
    return bytes(rng.randrange(256) for _ in range(4))


def blend_experiments(fmt: str, mode: str, option: str, methods: list, restore: list) -> dict:
    rng = random.Random(7)
    src = _sources(rng)
    dst = [_destination(rng, fmt) for _ in range(SIDE)]
    state, state_restore = blend_state(mode)
    return {
        'id': f'bff-{fmt}-{mode}-{option}',
        'kind': 'readback',
        'target': {'format': fmt, 'width': SIDE, 'height': SIDE},
        'fill': protocol.b64(b''.join(dst[y] for y in range(SIDE) for _ in range(SIDE))),
        'program': columns.program(src),
        'methods': methods + state,
        'restore': state_restore + restore,
        'readback': 'full',
        'meta': {'family': 'blend-ff', 'mode': mode, 'option': option, 'src': src, 'dst': [d.hex() for d in dst]},
    }


def precision() -> list[dict]:
    out = []
    for option, (methods, restore) in OPTIONS.items():
        for fmt in STORE_FORMATS:
            for e in rop.generate((fmt,), ('noblend',)):
                extra = {'methods': methods, 'restore': restore} if methods else {}
                out.append(e | {'id': f'{e["id"]}-{option}', 'meta': e['meta'] | {'option': option}} | extra)
        for fmt in BLEND_FORMATS:
            out += [blend_experiments(fmt, mode, option, methods, restore) for mode in MODES]
    for option, (methods, restore) in SRGB_OPTIONS.items():
        for e in rop.generate(('bgra8unorm',), ('noblend',)):
            out.append(
                e
                | {
                    'id': f'{e["id"].replace("bgra8unorm", "bgra8unorm_srgb")}-{option}',
                    'target': e['target'] | {'format': 'bgra8unorm_srgb'},
                    'methods': methods,
                    'restore': restore,
                    'meta': e['meta'] | {'option': option},
                }
            )
    return out


def metal_unorm(v: float, bits: int) -> int:
    """Metal's store: v * top rounded to nearest, half up, after clamping to [0, 1] (NaN to 0)."""
    top = (1 << bits) - 1
    if math.isnan(v) or v <= 0:
        return 0
    return top if v >= 1 else math.floor(Fraction(v) * top + Fraction(1, 2))


def _texels(result: dict) -> bytes:
    return protocol.unb64((result.get('texels') or {}).get('data') or '')


def analyse_precision(experiments: list[dict], results: dict[str, dict]) -> list[dict]:
    """Give, per experiment, whether it ran, whether its texels equal the base run's, and how many match the models."""
    by_id = {e['id']: e for e in experiments}
    rows = []
    for e in experiments:
        result = results.get(e['id']) or {}
        option = e['meta']['option']
        base_id = e['id'].replace(f'-{option}', '-base')
        row = {'id': e['id'], 'option': option, 'status': result.get('status')}
        if base_id in results and base_id != e['id']:
            row['same_as_base'] = _texels(result) == _texels(results[base_id])
        fmt = e['target']['format']
        if e['meta'].get('family') == 'rop' and fmt in texels.UNORM_BITS:
            seen = rop.readings(e, result)
            bits = texels.UNORM_BITS[fmt]
            row['gm20b'] = sum(rop.store(v, fmt, lane, 'gm20b') == got for v, lane, got in seen)
            row['metal'] = sum(metal_unorm(v, bits[lane]) == got for v, lane, got in seen)
            row['checked'] = len(seen)
        rows.append(row)
    del by_id
    return rows


def _one_target(e: dict) -> dict:
    """Give the experiment with colour 0 bound alone and a pixel header without MRT_ENABLE (bit 14): one output."""
    sph = bytearray(protocol.unb64(e['program']['sph']))
    sph[1] &= ~(1 << 6)
    return e | {'targets': 1, 'program': e['program'] | {'sph': protocol.b64(bytes(sph))}}


def _iterated_experiment(eid: str, methods: list, restore: list, meta: dict) -> dict:
    rng = random.Random(1)
    src = [(0.25, 0.5, 0.75, 1.0), (1.0, 0.0, 0.0, 0.5)] + [
        tuple(rng.randrange(256) / 255 for _ in range(4)) for _ in range(14)
    ]
    dst = [(0.5, 0.25, 0.125, 1.0), (0.0, 1.0, 0.0, 0.5)] + [
        tuple(rng.randrange(256) / 255 for _ in range(4)) for _ in range(14)
    ]
    fill = b''.join(texels.encode(dst[y], 'bgra8unorm') for y in range(16) for _ in range(16))
    e = {
        'id': eid,
        'kind': 'readback',
        'target': {'format': 'bgra8unorm', 'width': 16, 'height': 16},
        'fill': protocol.b64(fill),
        'program': columns.program(src),
        'methods': methods,
        'restore': restore,
        'readback': 'full',
        'meta': {'family': 'iterated', 'src': src, 'dst': dst} | meta,
    }
    return _one_target(e)


IB_RESTORE = [[0x0DD0, 0], [0x1360, 0], [0x12E4, 1], [0x02F4, 0]]


def primer(e: dict) -> dict:
    """Give a draw writing `e`'s fill rows into the same target, blend and iterated blend off."""
    fill, width = protocol.unb64(e['fill']), e['target']['width']
    rows = [texels.decode(fill[4 * width * y : 4 * width * y + 4], 'bgra8unorm') for y in range(e['target']['height'])]
    p = {k: v for k, v in e.items() if k not in ('id', 'meta', 'methods', 'restore', 'program')}
    p |= {'id': ('pr-' + e['id'])[:64], 'program': columns.program(rows, columns.POSITION_Y)}
    return _one_target(p | {'methods': [[0x0DD0, 0], [0x1360, 0]], 'restore': [[0x1360, 0]], 'meta': {'primer': True}})


def _global_blend(src: int = 0x4001, dst: int = 0x4000) -> list[list[int]]:
    return [
        [0x12E4, 0],
        [0x133C, 1],
        [0x1340, FUNC_ADD],
        [0x1344, src],
        [0x1348, dst],
        [0x134C, FUNC_ADD],
        [0x1350, src],
        [0x1358, dst],
    ]


def _program(words: list[int], passes: int | None = None, enable: int = 1) -> list[list[int]]:
    load = [[0x0DF0, 0]] + [[0x0DF4, w] for w in words]
    return [[0x02F4, 0], *_global_blend(), [0x1360, 1], *load, [0x0DD4, passes or len(words)], [0x0DD0, enable]]


def microcode(path: Path) -> dict[str, list[list[int]]]:
    """Give each program of a rung-3 microcode table as its (method, value) pairs in push order."""
    programs: dict[str, list[list[int]]] = {}
    with path.open() as f:
        for row in csv.DictReader(f, delimiter='\t'):
            programs.setdefault(row['name'], []).append([int(row['method'], 16), int(row['value'], 16)])
    return {name: ibprogram.renumbered(methods) for name, methods in programs.items()}


def iterated(table: Path | None = None) -> list[dict]:
    add = blend.instruction(
        {
            'test_cc': 'TRUE',
            'operation': 'ADD_PRODUCTS',
            'operand_a': 'SRC_RGB',
            'operand_b': 'ONE',
            'operand_c': 'DEST_RGB',
            'operand_d': 'ONE',
            'pass_output': 'TEMP0',
        }
    )
    mov = blend.instruction(
        {
            'test_cc': 'TRUE',
            'operation': 'ADD_PRODUCTS',
            'operand_a': 'SRC_RGB',
            'operand_b': 'ONE',
            'operand_c': 'DEST_RGB',
            'operand_d': 'ZERO',
            'pass_output': 'TEMP0',
        }
    )
    to_alpha = blend.instruction(
        {'test_cc': 'TRUE', 'operation': 'ADD', 'operand_b': 'SRC_AAA', 'operand_d': 'DEST_AAA'}
        | {'output_swizzle': 'R_TO_A', 'pass_output': 'TEMP1'}
    )
    keep = blend.instruction({'test_cc': 'TRUE', 'operation': 'ADD', 'operand_b': 'TEMP0_RGB', 'operand_d': 'ZERO'})
    setups = {
        'ib1t-control-ff': ([*_global_blend(0x4001, 0x4001), [0x1360, 1]], {'expect': 'add'}),
        'ib1t-mov': (_program([mov]), {'expect': 'src'}),
        'ib1t-add': (_program([add]), {'expect': 'add'}),
        'ib1t-add-rgba': (_program([add, to_alpha, keep], enable=3), {'expect': 'add'}),
        'ib1t-add-noblend0': ([m for m in _program([add]) if m[0] != 0x1360], {'expect': 'add'}),
        'ib1t-two-passes': (_program([add, mov], passes=2), {'expect': 'src'}),
        'ib1t-mrt-off': ([[0x0FAC, 0], *_program([add])], {'expect': 'add'}),
    }
    restore_mrt = {'ib1t-mrt-off': [[0x0FAC, 1]]}
    out = [
        _iterated_experiment(eid, methods, IB_RESTORE + restore_mrt.get(eid, []), meta)
        for eid, (methods, meta) in setups.items()
    ]
    if table is not None:
        for name, methods in microcode(table).items():
            eid = 'ib1t-' + ''.join(c if c.isalnum() else '-' for c in name.lower())[:58]
            out.append(_iterated_experiment(eid, methods, IB_RESTORE, {'program': name}))
    return [x for e in out for x in (primer(e), e)]


def analyse_iterated(experiments: list[dict], results: dict[str, dict]) -> list[dict]:
    """Give, per experiment, whether the draw landed (texels moved off the fill) and the first few texels."""
    rows = []
    for e in experiments:
        if e.get('meta', {}).get('primer'):
            continue
        result = results.get(e['id']) or {}
        data = _texels(result)
        fill = protocol.unb64(e['fill'])
        rows.append(
            {
                'id': e['id'],
                'status': result.get('status'),
                'why': result.get('why', ''),
                'landed': bool(data) and data != fill,
                'texels': [data[i : i + 4].hex() for i in range(0, min(len(data), 16), 4)],
            }
        )
    return rows
