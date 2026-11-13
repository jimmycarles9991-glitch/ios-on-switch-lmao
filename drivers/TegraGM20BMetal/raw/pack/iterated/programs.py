"""Iterated-blend programs for Core Animation's in-shader blend functions (QuartzCore 24B119).

Each program follows the evaluation order of fixed_frag_lph_cph (out/agent-scratch/x1-glass/blend/frag_cph.ll);
the IR line of the formula is next to each builder. S and D are premultiplied. The final RGB is T0, written by the
last pass, which is NVN's convention in Ryujinx's AdvancedBlendFunctions.cs.
"""

from collections.abc import Callable

from ucode import Asm, Program, half_bits

E = 0x068E  # the 1.0e-4 guard QuartzCore uses in every max(x, e)
W601 = (0x34CD, 0x38B8, 0x2F0A)  # 0.300, 0.590, 0.110: lum() of the HSL modes and dlc
W709 = (0x32CD, 0x39B9, 0x2C9D)  # 0.2126, 0.7153, 0.0722: darken_sover / lighten_sover
QUARTER = half_bits(0.25)
FF_OVER = ('ff', 'ONE', '1-SRC_A')  # Sa + Da(1 - Sa), the W3C alpha every separable mode uses
FF_DST = ('ff', 'ZERO', 'ONE')
FF_ADD = ('ff', 'ONE', 'ONE')
UCODE = ('ucode',)


def splat(bits: int) -> tuple[int, int, int]:
    return (bits, bits, bits)


def base(asm: Asm, dest: str = 'PBR') -> None:
    """(1 - Sa) D + S (1 - Da), IR:3355-3366."""
    asm.mmadd(dest, 'DST', '1-SRC_A', 'SRC', '1-DST_A')


def finish_w3c(asm: Asm) -> None:
    base(asm)
    asm.add('T0', 'T0', 'PBR')


def dot(asm: Asm, x: str, k: int, dest: str) -> None:
    """Broadcast the dot product of x and K[k], the luminance idiom of Ryujinx's HSL programs (RRR, GGG, BBB)."""
    asm.mul('PBR.RRR', x, 'K', k=k)
    asm.madd('PBR.GGG', x, 'K', 'PBR', k=k)
    asm.madd(dest + '.BBB', x, 'K', 'PBR', k=k)


def reduce3(asm: Asm, op: Callable[[str, str, str], None], x: str, dest: str) -> None:
    """max3/min3 of x broadcast to every lane: two rotations through PBR."""
    asm.mov('PBR.GBR', x)
    op('PBR.GBR', 'PBR', x)
    op(dest + '.GBR', 'PBR', x)


def unpremultiply(asm: Asm, color: str, alpha: str, dest: str, k: int) -> None:
    """Divide color by max(alpha, e), IR:3373-3380; K[k] holds e in every lane."""
    asm.max('PBR', alpha, 'K', k=k)
    asm.rcp('PBR', 'PBR')
    asm.mul(dest, color, 'PBR')


def clamp_to_alpha(asm: Asm, flags: str, k_quarter: int) -> None:
    """Set rgb = min(rgb, a) for codes 32, 33, 37, 38, 40 (IR:3692-3729); 1.25 a with x; nothing with u."""
    if 'u' in flags:
        return
    asm.mmadd('PBR', 'DST_A', '1-SRC_A', 'SRC_A', '1-DST_A')
    asm.mmadd('PBR', 'SRC_A', 'DST_A', 'PBR', 'ONE')
    asm.min('PBR', 'PBR', 'ONE')
    if 'x' in flags:
        asm.madd('PBR', 'PBR', 'K', 'PBR', k=k_quarter)
    asm.min('T0', 'T0', 'PBR')


def ucode_alpha(asm: Asm, a: str, b: str, c: str, d: str) -> None:
    """Alpha through R_TO_A, then the final move NVN's PlusDarker ends with."""
    asm.mmadd('T1.R_TO_A', a, b, c, d)
    asm.mov('T0', 'T0')


def program(
    name: str,
    build: Callable[[Asm], None],
    consts: dict[int, tuple[int, int, int]] | None = None,
    alpha: tuple[str, ...] = FF_OVER,
    note: str = '',
) -> Program:
    asm = Asm()
    build(asm)
    return Program(name, asm.code, consts or {}, alpha, frozenset(), note)


def separable(name: str, flags: str = '') -> Program:  # noqa: C901, PLR0915  build holds one branch per blend mode
    """Codes 26-41 and 60 that only need S and D."""
    consts: dict[int, tuple[int, int, int]] = {}
    clamp = name in ('cdg', 'cbn', 'sbt', 'div', 'llt')
    if clamp and 'x' in flags:
        consts[1] = splat(QUARTER)

    def build(asm: Asm) -> None:  # noqa: C901, PLR0912, PLR0915  one branch per blend mode, its IR lines' passes
        if name == 'mpy':  # IR:3134-3146
            asm.add('PBR', '1-SRC_A', 'SRC')
            asm.mmadd('T0', 'PBR', 'DST', 'SRC', '1-DST_A')
            return
        if name == 'pdi':  # IR:3067-3073
            asm.add('PBR', 'SRC', 'DST')
            asm.sub('T0', 'PBR', 'SRC_A')
            return
        if name in ('drk', 'ltn'):  # IR:3475-3493
            asm.mul('T0', 'DST', 'SRC_A')
            asm.mul('PBR', 'SRC', 'DST_A')
            (asm.min if name == 'drk' else asm.max)('T0', 'T0', 'PBR')
        elif name == 'dif':  # IR:3567-3576, |x| as max - min so no predication is needed
            asm.mul('T1', 'DST', 'SRC_A')
            asm.mul('PBR', 'SRC', 'DST_A')
            asm.max('T0', 'T1', 'PBR')
            asm.mul('PBR', 'SRC', 'DST_A')
            asm.min('T1', 'T1', 'PBR')
            asm.sub('T0', 'T0', 'T1')
        elif name == 'ovl':  # IR:3452-3473: Dc >= Da/2 picks 2(SaDc - ScDc + ScDa) - SaDa
            asm.add('PBR', 'DST', 'DST')
            asm.sub('PBR.CC', 'PBR', 'DST_A')
            asm.mmadd('T0', 'SRC', 'DST', 'SRC', 'DST', cc='LT')
            asm.sub('PBR', 'SRC_A', 'SRC', cc='GE')
            asm.mmadd('T0', 'PBR', 'DST', 'SRC', 'DST_A', cc='GE')
            asm.add('T0', 'T0', 'T0', cc='GE')
            asm.mmsub('T0', 'T0', 'ONE', 'SRC_A', 'DST_A', cc='GE')
        elif name == 'hlt':  # IR:3545-3565: Sc >= Sa/2 picks SaDa - 2(Da - Dc)(Sa - Sc)
            asm.add('PBR', 'SRC', 'SRC')
            asm.sub('PBR.CC', 'PBR', 'SRC_A')
            asm.mmadd('T0', 'SRC', 'DST', 'SRC', 'DST', cc='LT')
            asm.sub('T1', 'DST_A', 'DST', cc='GE')
            asm.add('T1', 'T1', 'T1', cc='GE')
            asm.sub('PBR', 'SRC_A', 'SRC', cc='GE')
            asm.mul('T1', 'T1', 'PBR', cc='GE')
            asm.mmsub('T0', 'SRC_A', 'DST_A', 'T1', 'ONE', cc='GE')
        elif name == 'pin':  # IR:3622-3641
            asm.add('PBR', 'SRC', 'SRC')
            asm.sub('PBR.CC', 'PBR', 'SRC_A')
            asm.mul('T1', 'SRC_A', 'DST')
            asm.mmadd('T2', 'SRC', 'DST_A', 'SRC', 'DST_A')
            asm.min('T0', 'T1', 'T2', cc='LT')
            asm.mmsub('PBR', 'T2', 'ONE', 'SRC_A', 'DST_A', cc='GE')
            asm.max('T0', 'T1', 'PBR', cc='GE')
        elif name == 'sbt':  # IR:3578-3586
            asm.mul('T0', 'SRC', 'DST_A')
            asm.mul('PBR', 'SRC_A', 'DST')
            asm.sub('T0', 'T0', 'PBR')
        elif name == 'div':  # IR:3588-3597
            asm.mul('T1', 'SRC', 'DST_A')
            asm.mul('PBR', 'SRC_A', 'DST')
            asm.max('PBR', 'PBR', 'K')
            asm.rcp('PBR', 'PBR')
            asm.mul('T0', 'T1', 'PBR')
        elif name == 'cdg':  # IR:3495-3507
            asm.mul('T1', 'SRC_A', 'SRC_A')
            asm.mul('T1', 'T1', 'DST')
            asm.sub('PBR', 'SRC_A', 'SRC')
            asm.max('PBR', 'PBR', 'K')
            asm.rcp('PBR', 'PBR')
            asm.mul('PBR', 'T1', 'PBR')
            asm.min('T0', 'SRC_A', 'PBR')
        elif name == 'cbn':  # IR:3509-3526
            asm.mul('T1', 'SRC_A', 'SRC_A')
            asm.sub('PBR', 'DST_A', 'DST')
            asm.mul('T1', 'T1', 'PBR')
            asm.max('PBR', 'SRC', 'K')
            asm.rcp('PBR', 'PBR')
            asm.mul('PBR', 'T1', 'PBR')
            asm.mmsub('T0', 'SRC_A', 'DST_A', 'PBR', 'ONE')
            asm.sub('PBR.CC', 'SRC', 'K')
            asm.mov('T0', 'ZERO', cc='LT')
        elif name == 'lbn':  # IR:3599-3608
            asm.sub('PBR', 'SRC_A', 'SRC')
            asm.mmsub('T0', 'SRC_A', 'DST', 'PBR', 'DST_A')
        elif name == 'llt':  # IR:3610-3620
            asm.add('PBR', 'SRC', 'SRC')
            asm.sub('PBR', 'SRC_A', 'PBR')
            asm.mmsub('T0', 'SRC_A', 'DST', 'PBR', 'DST_A')
        elif name == 'slt':  # IR:3528-3543
            asm.add('T1', 'SRC', 'SRC')
            asm.mul('T2', 'DST', 'DST')
            asm.sub('PBR', 'T1', 'SRC_A')
            asm.mul('T2', 'PBR', 'T2')
            asm.max('PBR', 'DST_A', 'K')
            asm.rcp('PBR', 'PBR')
            asm.mul('T2', 'T2', 'PBR')
            asm.mul('PBR', 'T1', 'DST')
            asm.sub('T0', 'PBR', 'T2')
        else:
            raise KeyError(name)
        finish_w3c(asm)
        if clamp:
            clamp_to_alpha(asm, flags, 1)

    if name in ('div', 'cdg', 'cbn', 'slt'):
        consts[0] = splat(E)
    alpha = FF_DST if name == 'pdi' else FF_OVER
    return program(name + ('.' + flags if flags else ''), build, consts, alpha)


def pld(f16: bool = False) -> Program:
    """IR:3118-3132. On BGRA8 the store clamp gives min(1, Sa + Da) from ONE/ONE; on RGBA16F it is microcode."""

    def build(asm: Asm) -> None:
        asm.add('T2', 'SRC_A', 'DST_A')
        asm.min('T1', 'T2', 'ONE')
        asm.sub('T2', 'T1', 'T2')
        asm.add('PBR', 'SRC', 'DST')
        asm.add('T0', 'T2', 'PBR')
        if f16:
            asm.mov('T1.R_TO_A', 'T1')
            asm.mov('T0', 'T0')

    return program('pld.f16' if f16 else 'pld', build, alpha=UCODE if f16 else FF_ADD)


def sds() -> Program:
    """IR:3229-3244: alpha is 1, which no fixed-function alpha factor can produce."""

    def build(asm: Asm) -> None:
        asm.mul('PBR', 'SRC_A', 'DST_A')
        asm.sub('PBR', 'ONE', 'PBR')
        asm.mmadd('T0', 'PBR', 'DST', 'SRC', 'DST_A')
        asm.mov('T1.R_TO_A', 'ONE')
        asm.mov('T0', 'T0')

    return program('sds', build, alpha=UCODE)


def adc_f16() -> Program:
    """IR:3262-3276. On BGRA8 the threshold is above 1, so the store clamp makes it ONE/ONE; this is RGBA16F."""

    def build(asm: Asm) -> None:
        reduce3(asm, asm.max, 'DST', 'PBR')
        asm.add('PBR', 'PBR', 'K', k=0)
        asm.max('T1', 'PBR', 'K', k=1)
        asm.add('PBR', 'SRC', 'DST')
        asm.min('T0', 'PBR', 'T1')
        asm.add('PBR', 'SRC_A', 'DST_A')
        asm.min('T1.R_TO_A', 'PBR', 'ONE')
        asm.mov('T0', 'T0')

    return program('adc.f16', build, {0: splat(0x3117), 1: splat(0x3CA3)}, UCODE)


def sover_by_luma(lighten: bool) -> Program:
    """darken_sover (IR:5719-5735) and lighten_sover (IR:5699-5716): S scaled by sat(L(D))^4, then source-over."""

    def build(asm: Asm) -> None:
        dot(asm, 'DST', 0, 'PBR')
        if lighten:
            asm.sub('PBR', 'ONE', 'PBR')
        asm.max('PBR', 'PBR', 'ZERO')
        asm.min('PBR', 'PBR', 'ONE')
        asm.mul('PBR', 'PBR', 'PBR')
        asm.mul('T1', 'PBR', 'PBR')
        asm.mul('T2', 'T1', 'SRC_A')
        asm.sub('PBR', 'ONE', 'T2')
        asm.mmadd('T0', 'PBR', 'DST', 'T1', 'SRC')
        asm.sub('PBR', 'ONE', 'T2')
        ucode_alpha(asm, 'PBR', 'DST_A', 'T2', 'ONE')

    return program('lso' if lighten else 'dso', build, {0: W709}, UCODE)
