"""The long programs: dlc, the four HSL modes, the colour matrix (vcm) and cli."""

from collections.abc import Sequence

from programs import FF_DST, FF_OVER, UCODE, W601, E, base, dot, program, reduce3, splat, unpremultiply
from ucode import Asm, Program, half_bits

LUM, EPS = 0, 1  # constant slots shared by dlc and the HSL programs


def dlc(unbounded: bool = False) -> Program:
    """IR:3246-3260 with set_lum_clip_on_format (IR:5563-5696), BGRA8 destination, no 's' or 'x'.

    min(d, 0) folds the lum(S) < lum(D) test into C: where it fails C is D exactly, and a BGRA8 D never trips the
    clip, so the output is D as QuartzCore's else branch gives.
    """

    def build(asm: Asm) -> None:
        dot(asm, 'SRC', LUM, 'T2')
        dot(asm, 'DST', LUM, 'PBR')
        asm.sub('PBR', 'T2', 'PBR')
        asm.min('PBR', 'PBR', 'ZERO')
        asm.add('T0', 'PBR', 'DST')
        if not unbounded:
            dot(asm, 'T0', LUM, 'T2')
            reduce3(asm, asm.min, 'T0', 'PBR')
            asm.sub('T1', 'ZERO', 'PBR')
            reduce3(asm, asm.max, 'T0', 'PBR')
            asm.sub('PBR', 'PBR', 'ONE')
            asm.max('PBR.CC', 'T1', 'PBR')
            asm.add('PBR', 'T2', 'T1', cc='GT')
            asm.max('PBR', 'PBR', 'K', k=EPS, cc='GT')
            asm.rcp('PBR', 'PBR', cc='GT')
            asm.mul('T1', 'T2', 'PBR', cc='GT')
            asm.sub('PBR', 'T0', 'T2', cc='GT')
            asm.madd('T0', 'T1', 'PBR', 'T2', cc='GT')
        asm.mov('T0', 'T0')

    return program('dlc.u' if unbounded else 'dlc', build, {LUM: W601, EPS: splat(E)}, FF_DST)


def set_lum(asm: Asm) -> None:
    """UberShader::set_lum (IR:5402-5462) with c in T1 and l in T2; result in T0.

    QuartzCore picks the clip formula by the sign of l - lum(c); for c in [0, 1] that sign is fixed by which bound
    C leaves, so the two clips run one after the other as in NVN's HSL programs.
    """
    dot(asm, 'T1', LUM, 'PBR')
    asm.sub('PBR', 'T2', 'PBR')
    asm.add('T1', 'PBR', 'T1')
    dot(asm, 'T1', LUM, 'T2')
    reduce3(asm, asm.max, 'T1', 'T0')
    asm.sub('PBR.CC', 'T0', 'ONE')
    asm.sub('PBR', 'T0', 'T2', cc='GT')
    asm.max('PBR', 'PBR', 'K', k=EPS, cc='GT')
    asm.rcp('T0', 'PBR', cc='GT')
    asm.sub('PBR', 'ONE', 'T2', cc='GT')
    asm.mul('T0', 'PBR', 'T0', cc='GT')
    asm.sub('PBR', 'T1', 'T2', cc='GT')
    asm.madd('T0', 'T0', 'PBR', 'T2', cc='GT')
    asm.mov('T0', 'T1', cc='LE')
    asm.mov('PBR.GBR', 'T1')
    asm.min('PBR.GBR', 'PBR', 'T1')
    asm.min('PBR.GBR.CC', 'PBR', 'T1')
    asm.sub('PBR', 'T2', 'PBR', cc='LT')
    asm.max('PBR', 'PBR', 'K', k=EPS, cc='LT')
    asm.rcp('PBR', 'PBR', cc='LT')
    asm.mul('T0', 'T2', 'PBR', cc='LT')
    asm.sub('PBR', 'T1', 'T2', cc='LT')
    asm.madd('T0', 'T0', 'PBR', 'T2', cc='LT')


def set_sat(asm: Asm) -> None:
    """UberShader::set_sat (IR:5471-5522) on c in T2 with the target saturation in T1; result in T2.

    (c - min) s / (max - min) gives QuartzCore's one-hot result in every lane: s on the max lane, 0 on the min lane.
    """
    reduce3(asm, asm.min, 'T2', 'PBR')
    asm.sub('T2', 'T2', 'PBR')
    asm.mov('PBR.GBR', 'T2')
    asm.max('PBR.GBR', 'PBR', 'T2')
    asm.max('PBR.GBR.CC', 'PBR', 'T2')
    asm.rcp('T0', 'PBR', cc='GT')
    asm.mul('T2', 'T2', 'T1', cc='GT')
    asm.mul('T2', 'T2', 'T0', cc='GT')
    asm.mov('T2', 'ZERO', cc='LE')


def saturation(asm: Asm, color: str, alpha: str) -> None:
    """sat(color / alpha) into T1 (IR:5525-5545); uses T2."""
    unpremultiply(asm, color, alpha, 'T2', EPS)
    reduce3(asm, asm.min, 'T2', 'T1')
    reduce3(asm, asm.max, 'T2', 'PBR')
    asm.sub('T1', 'PBR', 'T1')


def finish_hsl(asm: Asm) -> None:
    """B Sa Da + base, IR:3422-3428."""
    asm.mul('PBR', 'SRC_A', 'DST_A')
    asm.mul('T0', 'T0', 'PBR')
    base(asm)
    asm.add('T0', 'T0', 'PBR')


def hsl(name: str) -> Program:
    """IR:3372-3428 (codes 54-57)."""

    def build(asm: Asm) -> None:
        if name in ('col', 'lum'):
            c_of, l_of = ('SRC', 'SRC_A'), ('DST', 'DST_A')
            if name == 'lum':
                c_of, l_of = l_of, c_of
            unpremultiply(asm, *c_of, 'T1', EPS)
        else:
            sat_of, sat_on = (('DST', 'DST_A'), ('SRC', 'SRC_A'))[:: 1 if name == 'hue' else -1]
            saturation(asm, *sat_of)
            unpremultiply(asm, *sat_on, 'T2', EPS)
            set_sat(asm)
            asm.mov('T1', 'T2')
            l_of = ('DST', 'DST_A')
        unpremultiply(asm, *l_of, 'T2', EPS)
        dot(asm, 'T2', LUM, 'T2')
        set_lum(asm)
        finish_hsl(asm)

    return program(name, build, {LUM: W601, EPS: splat(E)}, FF_OVER)


def vcm(m: Sequence[Sequence[float]], p3: float = 0.0, p4: float = 0.0) -> Program:
    """vibrant_color_matrix_sover (IR:6074-6179). m: five RGBA rows of the matrix uniform; p3, p4: its clamp pair.

    K0..K4 = rows' rgb, K5 = (M0.a, M1.a, M2.a), K6 = (M3.a, M4.a, e), K7 = (p3, -0.5, 0).
    The alpha row reaches every lane through RRR/GGG swizzles on K5 and K6.
    """
    h = half_bits
    consts = {j: tuple(h(x) for x in m[j][:3]) for j in range(5)}
    consts[5] = tuple(h(m[j][3]) for j in range(3))
    consts[6] = (h(m[3][3]), h(m[4][3]), E)
    clamp = 'none' if p3 <= 0 else 'scale' if p4 > 0 else 'clip'
    if clamp != 'none':
        consts[7] = (h(p3), h(-0.5), 0)

    def build(asm: Asm) -> None:
        asm.max('PBR.BBB', 'DST_A', 'K', k=6)
        asm.rcp('PBR', 'PBR')
        asm.mul('T0', 'DST', 'PBR')
        asm.mov('PBR.RRR', 'T0')
        asm.mul('T1', 'PBR', 'K', k=0)
        for j, swz in ((1, 'GGG'), (2, 'BBB')):
            asm.mov('PBR.' + swz, 'T0')
            asm.madd('T1', 'PBR', 'K', 'T1', k=j)
        asm.madd('T1', 'DST_A', 'K', 'T1', k=3)
        asm.add('T1', 'T1', 'K', k=4)
        dot(asm, 'T0', 5, 'PBR')
        asm.madd('PBR.RRR', 'DST_A', 'K', 'PBR', k=6)
        asm.add('PBR.GGG', 'PBR', 'K', k=6)
        asm.max('PBR', 'PBR', 'ZERO')
        asm.min('T2', 'PBR', 'ONE')
        asm.mul('T1', 'T1', 'T2')
        asm.mul('T1', 'T1', 'SRC_A')
        asm.mul('T2', 'T2', 'SRC_A')
        if clamp != 'none':  # IR:6115-6168
            asm.max('PBR.BBB', 'T2', 'K', k=6)
            asm.rcp('PBR', 'PBR')
            asm.mul('T1', 'T1', 'PBR')
            if clamp == 'clip':
                asm.mov('PBR.GGG', 'K', k=7)
                asm.max('T1', 'T1', 'PBR')
                asm.mov('PBR.RRR', 'K', k=7)
                asm.min('T1', 'T1', 'PBR')
            else:
                reduce3(asm, asm.max, 'T1', 'T0')
                asm.sub('PBR.RRR.CC', 'T0', 'K', k=7)
                asm.rcp('T0', 'T0', cc='GT')
                asm.mov('PBR.RRR', 'K', k=7, cc='GT')
                asm.mul('T0', 'T0', 'PBR', cc='GT')
                asm.mul('T1', 'T1', 'T0', cc='GT')
            asm.mul('T1', 'T1', 'T2')
        asm.sub('PBR', 'ONE', 'T2')
        asm.mmadd('T0', 'PBR', 'DST', 'T1', 'ONE')
        asm.sub('PBR', 'ONE', 'T2')
        asm.mmadd('T1.R_TO_A', 'PBR', 'DST_A', 'T2', 'ONE')
        asm.mov('T0', 'T0')

    return program(
        'vcm' + ('' if clamp == 'none' else '.' + clamp),
        build,
        consts,
        UCODE,
        'constants are the per-draw matrix uniform; the clamp branch is chosen by p3, p4',
    )


CLI_K = {  # IR:3278-3350, all compile-time constants
    0: (0x3FA9, 0xAD71, 0xAD71),
    1: (0xB493, 0x3EDB, 0xB493),
    2: (0xA76D, 0xA76D, 0x3FE2),
    3: (0x3D63, 0xAAC9, 0xAAC9),
    4: (0xB1BA, 0x3CE2, 0xB1BA),
    5: (0xA49C, 0xA49C, 0x3D87),
    6: (0x3B33, 0x3266, 0xBA00),  # 0.9 and 0.2 offsets, -0.75 lower clamp factor
    7: (half_bits(1.0864000320434570 - 1), 0, 0),  # upper clamp factor minus 1: a + 0.0864 a keeps fp16 precision
}


def cli_matrix(asm: Asm, src: str, acc: str, k0: int, offset_swz: str) -> None:
    asm.mov('PBR.GGG', src)
    asm.mul(acc, 'PBR', 'K', k=k0 + 1)
    asm.mov('PBR.' + offset_swz, 'K', k=6)
    asm.add(acc, acc, 'PBR')
    asm.mov('PBR.RRR', src)
    asm.madd(acc, 'PBR', 'K', acc, k=k0)
    asm.mov('PBR.BBB', src)
    asm.madd(acc, 'PBR', 'K', acc, k=k0 + 2)


def cli_alpha_times(asm: Asm, lane_swz: str, alpha: str) -> None:
    asm.mov('PBR.' + lane_swz, 'SRC')
    asm.mul('PBR', 'PBR', alpha)


def cli_clamp(asm: Asm, x: str, alpha_reg: str, lane_swz: str) -> None:
    if alpha_reg == 'T2' and lane_swz == 'RRR':
        asm.mul('PBR.BBB', 'T2', 'K', k=6)
    else:
        cli_alpha_times(asm, lane_swz, alpha_reg)
        asm.mul('PBR.BBB', 'PBR', 'K', k=6)
    asm.max(x, x, 'PBR')
    if alpha_reg == 'T2' and lane_swz == 'RRR':
        asm.madd('PBR.RRR', 'T2', 'K', 'T2', k=7)
    else:
        cli_alpha_times(asm, lane_swz, alpha_reg)
        asm.madd('PBR.RRR', 'PBR', 'K', 'PBR', k=7)
    asm.min(x, x, 'PBR')


def cli() -> Program:
    def build(asm: Asm) -> None:
        cli_matrix(asm, 'DST', 'T1', 0, 'RRR')
        asm.mov('PBR.RRR', 'SRC')
        asm.mul('T2', 'PBR', 'DST_A')
        asm.mov('PBR.RRR', 'SRC')
        asm.mul('T1', 'T1', 'PBR')
        cli_clamp(asm, 'T1', 'T2', 'RRR')
        asm.sub('PBR', 'ONE', 'T2')
        asm.madd('T1', 'PBR', 'DST', 'T1')
        asm.sub('PBR', 'ONE', 'T2')
        asm.madd('T2', 'PBR', 'DST_A', 'T2')
        asm.mov('PBR.GGG', 'SRC')
        asm.add('T0', 'T2', 'PBR')
        asm.max('PBR', 'T0', 'ZERO')
        asm.min('T2', 'PBR', 'ONE')
        asm.sub('PBR', 'T2', 'T0')
        asm.add('T1', 'PBR', 'T1')
        cli_matrix(asm, 'T1', 'T0', 3, 'GGG')
        asm.mov('PBR.BBB', 'SRC')
        asm.mul('T0', 'T0', 'PBR')
        cli_clamp(asm, 'T0', 'T2', 'BBB')
        cli_alpha_times(asm, 'BBB', 'T2')
        asm.sub('PBR', 'ONE', 'PBR')
        asm.mmadd('T0', 'PBR', 'T1', 'T0', 'ONE')
        asm.mov('PBR.BBB', 'SRC')
        asm.mul('T1', 'PBR', 'T2')
        asm.sub('PBR', 'ONE', 'T1')
        asm.mmadd('PBR', 'PBR', 'T2', 'T1', 'ONE')
        asm.min('T1.R_TO_A', 'PBR', 'ONE')
        asm.mov('T0', 'T0')

    return program('cli', build, dict(CLI_K), UCODE)
