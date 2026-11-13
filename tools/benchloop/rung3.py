"""Fetch rung 3's iterated blend programs as bench tables, each against QuartzCore's formula.

The pack's microcode (raw/pack/iterated/table.py) is drawn on the chip over the bench's source and destination colours.

    bazel run //tools/benchloop:rung3 -- <out dir>    writes <out dir>/<program>.json, an iterated-blend/1 table each
    bazel run //tools/benchloop -- blend <out dir>/*.json

Each table sets the program as a fetch-free form's draw does (raw_fetch_free.h iteratedBlendWords): the per-target blend
off, the common block's colour ADD ONE ZERO and the program's alpha factors, SET_BLEND(0) on, then the program. A
program whose formula needs a per-lane condition the formula language lacks (overlay, hard and pin light, colour burn,
the HSL modes, darker colour, the clip constant) is left out: gpu.raw.fetch_rung3 draws those against the storage form.
"""

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'drivers/TegraGM20BMetal/raw/pack/iterated'))

import table

E = '0.0001'
OVER_A = 'src.a + dst.a - src.a * dst.a'
W3C_TAIL = 'dst.rgb * (1 - src.a) + src.rgb * (1 - dst.a)'
SEPARABLE = {
    'drk': 'min(src.a * dst.rgb, src.rgb * dst.a)',
    'ltn': 'max(src.a * dst.rgb, src.rgb * dst.a)',
    'dif': 'abs(src.a * dst.rgb - src.rgb * dst.a)',
    'lbn': 'src.a * dst.rgb - (src.a - src.rgb) * dst.a',
    'slt': f'2 * src.rgb * dst.rgb - (2 * src.rgb - src.a) * dst.rgb * dst.rgb / max(dst.a, {E})',
}
# the blend functions QuartzCore clamps to rgb <= alpha (its 1.25 alpha with extended range, unclamped unbounded)
CLAMPED = {
    'cdg': f'min(src.a, src.a * src.a * dst.rgb / max(src.a - src.rgb, {E}))',
    'sbt': 'src.rgb * dst.a - src.a * dst.rgb',
    'div': f'src.rgb * dst.a / max(src.a * dst.rgb, {E})',
    'llt': 'src.a * dst.rgb - (src.a - 2 * src.rgb) * dst.a',
}
OTHER = {
    'mpy': ('src.rgb * (1 - dst.a) + ((1 - src.a) + src.rgb) * dst.rgb', 'saturate(src.a * (1 - dst.a) + dst.a)'),
    'pdi': ('src.rgb + dst.rgb - src.a', 'dst.a'),
    'pld': ('src.rgb + dst.rgb + min(1, src.a + dst.a) - (src.a + dst.a)', 'min(1, src.a + dst.a)'),
}
PER_TARGET, SEPARATE, COLOR_OP, COLOR_SRC, COLOR_DST = 0x12E4, 0x133C, 0x1340, 0x1344, 0x1348
ALPHA_OP, ALPHA_SRC, ALPHA_DST = 0x134C, 0x1350, 0x1358
OGL_ADD, OGL_ONE, OGL_ZERO = 0x8006, 0x4001, 0x4000


def formulas(name: str) -> tuple[str, str] | None:
    mode, _, flag = name.partition('.')
    if mode in OTHER and not flag:
        return OTHER[mode]
    if mode in SEPARABLE and not flag:
        return f'{SEPARABLE[mode]} + {W3C_TAIL}', f'saturate({OVER_A})'
    if mode in CLAMPED:
        rgb = f'{CLAMPED[mode]} + {W3C_TAIL}'
        limit = {'': f'saturate({OVER_A})', 'x': f'1.25 * saturate({OVER_A})'}.get(flag)
        return (f'min({rgb}, {limit})' if limit else rgb), f'saturate({OVER_A})'
    return None


def bench_table(name: str) -> dict | None:
    found = formulas(name)
    prog = table.PROGRAMS[name]
    if not found or prog.alpha[0] != 'ff':
        return None
    record = table.record(prog)
    methods = [
        [PER_TARGET, 0],
        [SEPARATE, 1],
        [COLOR_OP, OGL_ADD],
        [COLOR_SRC, OGL_ONE],
        [COLOR_DST, OGL_ZERO],
        [ALPHA_OP, OGL_ADD],
        [ALPHA_SRC, record['alpha_source']],
        [ALPHA_DST, record['alpha_dest']],
        [0x1360, 1],
    ]
    constants = [list(record['constants'].get(j, (0, 0, 0))) for j in range(len(record['constants']))]
    return {
        'format': 'iterated-blend/1',
        'name': f'rung3-{name}',
        'passes': len(record['words']),
        'alpha': False,
        'instructions': [f'0x{w:08x}' for w in record['words']],
        'constants': constants,
        'methods': methods,
        'restore': [[PER_TARGET, 1]],
        'rgb': found[0],
        'a': found[1],
        'tolerance': 2,
    }


def main() -> None:
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    written = []
    for name in sorted(table.PROGRAMS):
        t = bench_table(name)
        if t:
            (out / f'{name}.json').write_text(json.dumps(t, indent=1) + '\n')
            written.append(name)
    print(f'rung3: {len(written)} tables: {" ".join(written)}')


if __name__ == '__main__':
    main()
