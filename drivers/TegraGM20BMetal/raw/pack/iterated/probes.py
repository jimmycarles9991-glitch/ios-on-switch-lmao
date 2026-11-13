# ruff: noqa: N803  S and D are the blend's source and destination pixels, as in reference.py
"""Programs d14 runs before any blend mode: each isolates one assumption of emu.MODEL.

expect(S, D) is the colour the model predicts before the store; 'otherwise' says what a mismatch means.
"""

from collections.abc import Callable, Sequence

from programs import FF_DST, FF_OVER, UCODE
from ucode import Asm, Program, half_bits

type Expectation = Callable[[Sequence[float], Sequence[float]], tuple[float, ...]]


def nvn_multiply(asm: Asm) -> None:
    """Ryujinx GenUncorrelatedMultiplyPremul (MIT): NVN's words, which Ryujinx matches by hash to games' uploads."""
    asm.mul('T0', 'SRC', 'DST')
    asm.mmadd('PBR', 'SRC', '1-DST_A', 'DST', '1-SRC_A')
    asm.add('T0', 'T0', 'PBR')


def lanes(f: Callable[[float, float, int, Sequence[float], Sequence[float]], float]) -> Expectation:
    """Build the expectation per lane from (s, d, lane, S, D); alpha is Da (ZERO, ONE)."""
    return lambda S, D: (*(f(S[i], D[i], i, S, D) for i in range(3)), D[3])


K_QUARTERS = {0: tuple(half_bits(x) for x in (0.25, 0.5, 0.75))}
K_RANGE = {0: tuple(half_bits(x) for x in (2.0, -1.0, 0.5))}

SPECS = [  # name, build, expect, otherwise, constants, alpha
    (
        'mmadd',
        lambda a: a.mmadd('T0', 'SRC', 'ONE', 'DST', 'ONE'),
        lanes(lambda s, d, _i, _S, _D: s + d),
        'MMADD is not A*B + C*D, or the output is not T0',
        None,
        FF_DST,
    ),
    (
        'mmsub',
        lambda a: a.mmsub('T0', 'DST', 'ONE', 'SRC', 'ONE'),
        lanes(lambda s, d, _i, _S, _D: d - s),
        'MMSUB is not A*B - C*D',
        None,
        FF_DST,
    ),
    (
        'min',
        lambda a: a.min('T0', 'SRC', 'DST'),
        lanes(lambda s, d, _i, _S, _D: min(s, d)),
        'MIN reads other slots (min(A*B, C*D) gives min(S*D, 0) with the default C, D)',
        None,
        FF_DST,
    ),
    ('max', lambda a: a.max('T0', 'SRC', 'DST'), lanes(lambda s, d, _i, _S, _D: max(s, d)), 'as min', None, FF_DST),
    (
        'add_bd',
        lambda a: a.add('T0', 'DST', 'ZERO'),
        lanes(lambda _s, d, _i, _S, _D: d),
        'ADD is A + C (gives 2S with A = C = SRC) or A + B (S + D)',
        None,
        FF_DST,
    ),
    (
        'sub_bd',
        lambda a: a.sub('T0', 'ONE', 'DST'),
        lanes(lambda _s, d, _i, _S, _D: 1 - d),
        'SUB is A - C (gives 0)',
        None,
        FF_DST,
    ),
    (
        'rcp_pbr',
        lambda a: (a.rcp('PBR', 'DST_A'), a.mul('T0', 'DST', 'PBR')),
        lanes(lambda _s, d, _i, _S, D: d / D[3] if D[3] else float('nan')),
        'RCP is not 1/A, or PBR does not hold the previous result; draw with Da > 0',
        None,
        FF_DST,
    ),
    (
        'pbr_after_temp',
        lambda a: (a.mmadd('T1', 'SRC', 'ONE', 'DST', 'ZERO'), a.mov('T0', 'PBR')),
        lanes(lambda s, _d, _i, _S, _D: s),
        'PBR only follows passes whose PASS_OUTPUT is NONE',
        None,
        FF_DST,
    ),
    (
        'swz_rrr',
        lambda a: (a.mov('T0.RRR', 'SRC'), a.mov('T0', 'T0')),
        lanes(lambda _s, _d, _i, S, _D: S[0]),
        'the swizzle does not broadcast the result lane',
        None,
        FF_DST,
    ),
    (
        'swz_gbr',
        lambda a: (a.mov('T0.GBR', 'SRC'), a.mov('T0', 'T0')),
        lanes(lambda _s, _d, i, S, _D: S[(i + 1) % 3]),
        '(Sb, Sr, Sg) means GBR rotates the other way; reduce3 works either way',
        None,
        FF_DST,
    ),
    (
        'mask_r',
        lambda a: (a.mov('T0', 'ZERO'), a.mov('T0.R', 'ONE')),
        lanes(lambda _s, _d, i, _S, _D: 1.0 if i == 0 else 0.0),
        'the write mask does not hold the other lanes',
        None,
        FF_DST,
    ),
    (
        'cc_lane',
        lambda a: (
            a.mov('T0', 'ONE'),
            a.sub('PBR.CC', 'SRC', 'DST'),
            a.mov('T1', 'ZERO'),
            a.mov('T0', 'ZERO', cc='LT'),
            a.mov('T0', 'T0'),
        ),
        lanes(lambda s, d, _i, _S, _D: 0.0 if s < d else 1.0),
        'CC is one value per pixel, does not survive a pass without SET_CC, or skipped lanes are written',
        None,
        FF_DST,
    ),
    (
        'clamp_low',
        lambda a: (a.sub('T1', 'SRC', 'ONE'), a.add('T0', 'T1', 'ONE')),
        lanes(lambda s, _d, _i, _S, _D: s),
        'on BGRA8: 1.0 means temps clamp negatives to 0 (pld, dlc, sbt, llt and the HSL clips need them)',
        None,
        FF_DST,
    ),
    (
        'clamp_high',
        lambda a: (a.add('T1', 'SRC', 'ONE'), a.sub('T0', 'T1', 'ONE')),
        lanes(lambda s, _d, _i, _S, _D: s),
        'on BGRA8: 0.0 means temps clamp at 1 (2Sc, RCP results and the 1e4 quotients of div/cdg/cbn need >1)',
        None,
        FF_DST,
    ),
    (
        'source_range',
        lambda a: (a.mul('PBR', 'SRC', 'K', k=0), a.add('T0', 'PBR', 'K', k=1)),
        lanes(lambda s, _d, _i, _S, _D: s / 4 + 0.5),
        'shader writes S outside [0, 1] (1.6, -0.4): 0.9 / 0.4 mean the unit sees it unclamped; 0.75 / 0.5 clamped',
        {0: (0x3400, 0x3400, 0x3400), 1: (0x3800, 0x3800, 0x3800)},
        FF_DST,
    ),
    (
        'k_fp16',
        lambda a: a.mov('T0', 'K'),
        lanes(lambda _s, _d, i, _S, _D: (0.25, 0.5, 0.75)[i]),
        'constants are not fp16 bit patterns',
        K_QUARTERS,
        FF_DST,
    ),
    (
        'k_range',
        lambda a: a.mul('T0', 'SRC', 'K'),
        lanes(lambda s, _d, i, _S, _D: s * (2.0, -1.0, 0.5)[i]),
        'constants or intermediate results are clamped to [0, 1] (read on RGBA16F)',
        K_RANGE,
        FF_DST,
    ),
    (
        'alpha_ff',
        lambda a: a.mov('T0', 'SRC'),
        lambda S, D: (*S[:3], S[3] + D[3] * (1 - S[3])),
        'ENABLE_RGB does not take alpha from SET_BLEND_ALPHA_*',
        None,
        FF_OVER,
    ),
    (
        'alpha_r_to_a',
        lambda a: (a.mov('T0', 'SRC'), a.mov('T1.R_TO_A', 'K'), a.mov('T0', 'T0')),
        lambda S, _D: (*S[:3], 0.25),
        'R_TO_A does not reach the alpha output',
        {0: (half_bits(0.25), 0, 0)},
        UCODE,
    ),
    (
        'pass_count',
        lambda a: (a.mov('T0', 'ONE'), a.mov('T0', 'ZERO')),
        lanes(lambda _s, _d, _i, _S, _D: 0.0),
        'push PASS = 1 as well: 1.0 then means COUNT is the pass count, 0.0 means COUNT + 1 passes run',
        None,
        FF_DST,
    ),
    (
        'precision',
        lambda a: (a.mul('T1', 'SRC', 'K'), a.sub('T0', 'T1', 'SRC'), *[a.add('T0', 'T0', 'T0') for _ in range(10)]),
        lanes(lambda s, _d, _i, _S, _D: s),
        'T1 = S K (K = 1 + 2^-10), (T1 - S) doubled ten times: fp16 temps give 0.25 for S = 0.3, fp32 temps 0.3',
        {0: (0x3C01, 0x3C01, 0x3C01)},
        FF_DST,
    ),
    (
        'nvn_multiply',
        nvn_multiply,
        lambda S, D: (
            *(S[i] * D[i] + S[i] * (1 - D[3]) + D[i] * (1 - S[3]) for i in range(3)),
            S[3] + D[3] * (1 - S[3]),
        ),
        "NVN's own program fails: the setup around the program is wrong",
        None,
        FF_OVER,
    ),
]


def build() -> list[tuple[Program, Expectation]]:
    out = []
    for name, body, expect, otherwise, consts, alpha in SPECS:
        asm = Asm()
        body(asm)
        out.append((Program('probe.' + name, asm.code, consts or {}, alpha, frozenset(), otherwise), expect))
    return out
