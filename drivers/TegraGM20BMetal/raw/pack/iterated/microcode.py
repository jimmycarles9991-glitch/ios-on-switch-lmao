"""Rung 3 microcode table: Core Animation blend function -> MAXWELL_B methods for HW Test d14.

    python3 microcode.py           write microcode.tsv (name, tags, method, value, decoded) and print a summary
    python3 microcode.py --check   also run every program against reference.py, and every probe, under emu.MODEL

TABLE[name] is the ordered (method, value) list to push. '.x' / '.u' are the variants for fc_extended_range /
fc_unbounded_range states, '.f16' the RGBA16F-target variants, 'probe.*' the model probes d14 runs first.
The vcm rows carry the identity matrix (out = c + (1 - c.a) D with c = (D.rgb / Da, Da) Da Sa) so a probe has a
fixed expectation; --check also runs them with a random matrix.
"""

import pathlib
import random
import sys
from collections.abc import Mapping, Sequence
from typing import TypedDict

import emu
import probes
import programs as p
import programs_long as pl
from reference import apple
from ucode import Insn, Program, half_bits, half_value

HERE = pathlib.Path(__file__).resolve().parent
IDENTITY = [[1.0 if i == j else 0.0 for i in range(4)] for j in range(4)] + [[0.0] * 4]
VCM_CLAMPS = ((0.0, 0.0), (0.8, 0.0), (0.8, 1.0))  # none, clip, scale


class CheckOptions(TypedDict, total=False):
    params: tuple[Sequence[Sequence[float]], float, float] | Mapping[int, Sequence[float]]
    unorm: bool
    tol: float


def vcm_programs(m: Sequence[Sequence[float]]) -> list[tuple[Program, CheckOptions]]:
    return [(pl.vcm(m, p3, p4), {'params': (m, half_value(half_bits(p3)), p4)}) for p3, p4 in VCM_CLAMPS]


def build(matrix: Sequence[Sequence[float]] = IDENTITY) -> list[tuple[Program, CheckOptions]]:
    progs: list[tuple[Program, CheckOptions]] = [
        (p.separable(name), {}) for name in ('mpy', 'pdi', 'drk', 'ltn', 'dif', 'ovl', 'hlt', 'pin', 'lbn', 'slt')
    ]
    progs += [
        (p.separable(name, flags), {}) for name in ('cdg', 'cbn', 'sbt', 'div', 'llt') for flags in ('', 'x', 'u')
    ]
    progs += [(p.pld(), {}), (p.pld(f16=True), {'unorm': False}), (p.sds(), {}), (p.adc_f16(), {'unorm': False})]
    progs += [(p.sover_by_luma(False), {}), (p.sover_by_luma(True), {})]
    progs += [(pl.dlc(), {}), (pl.dlc(unbounded=True), {})]
    progs += [(pl.hsl(name), {}) for name in ('hue', 'sat', 'col', 'lum')]
    progs += vcm_programs(matrix)
    k = {j: [half_value(b) for b in v] for j, v in pl.CLI_K.items()}
    k[7] = [k[7][0] + 1, 0, 0]
    progs.append((pl.cli(), {'params': k, 'tol': 1e-4}))
    for prog, _ in progs:
        prog.uses = emu.tags(prog)
    return progs


def all_programs() -> list[Program]:
    progs = [(prog, None) for prog, _ in probes.build()] + [(prog, None) for prog, _ in build()]
    for prog, _ in progs:
        prog.uses = emu.tags(prog)
    return [prog for prog, _ in progs]


TABLE = {prog.name: prog.methods() for prog in all_programs()}


def write_tsv(path: pathlib.Path) -> None:
    with open(path, 'w') as f:
        f.write('name\ttags\tmethod\tvalue\tdecoded\n')
        for prog in all_programs():
            tags = ','.join(sorted(prog.uses))
            for method, value in TABLE[prog.name]:
                text = Insn.decode(value) if method == 0x0DF4 else ''
                f.write(f'{prog.name}\t{tags}\t0x{method:04x}\t0x{value:08x}\t{text}\n')


def check_programs() -> None:
    rng = random.Random(7)
    sample = [[half_value(half_bits(rng.uniform(-0.6, 1.4))) for _ in range(4)] for _ in range(5)]
    print('name\tpasses\tconsts\talpha\ttags\texact\tfp16(1/255)')
    runs = [(prog, opts, '') for prog, opts in build()]
    runs += [(prog, opts, ' (random M)') for prog, opts in vcm_programs(sample)]
    for prog, opts, label in runs:
        params = opts.get('params')
        ok, exact, fp16, worst = emu.check(
            prog,
            lambda src, dst, name=prog.name, params=params: apple(name, src, dst, params),
            unorm=opts.get('unorm', True),
            tol=opts.get('tol', 1e-6),
        )
        tags = ','.join(sorted(emu.tags(prog) - {'ops', 'out_t0'}))
        print(
            f'{prog.name}{label}\t{len(prog.code)}\t{len(prog.consts)}\t{prog.alpha[0]}\t{tags}\t'
            f'{"ok" if ok else "FAIL"} {exact:.2g}\t{fp16:.2f}'
        )
        if not ok:
            print('  worst', worst, file=sys.stderr)


def check_probes() -> None:
    for prog, expect in probes.build():
        worst = 0.0
        for src, dst in emu.inputs(1, 500):
            if dst[3] == 0:
                continue
            got, want = emu.run(prog, src, dst), expect(src, dst)
            err = max(abs(a - b) for a, b in zip(got, want, strict=True))
            worst = max(worst, err)
        print(f'{prog.name}\t{len(prog.code)}\t{"ok" if worst < 1e-9 else "FAIL"}\t{prog.note}')


if __name__ == '__main__':
    write_tsv(HERE / 'microcode.tsv')
    print(f'{len(TABLE)} programs written to microcode.tsv')
    if '--check' in sys.argv:
        check_programs()
        check_probes()
