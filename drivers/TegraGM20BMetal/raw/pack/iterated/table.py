"""The Core Animation fragment blends a fetch-free form draws through the ROP's iterated blend (fetch rung 3).

Each program is named by QuartzCore's blend function code, with '.x' or '.u' for its fc_extended_range or
fc_unbounded_range form, and is written for an 8-bit unorm target. Left out: the RGBA16Float forms ('.f16') and the
colour matrix (vcm), whose constants are per-draw uniforms (microcode.py writes those for the bench).
"""

import programs as p
import programs_long as pl
from ucode import ENABLE_RGB, ENABLE_RGBA, OGL, Program

SEPARABLE = ('mpy', 'pdi', 'drk', 'ltn', 'dif', 'ovl', 'hlt', 'pin', 'lbn', 'slt')
RANGED = ('cdg', 'cbn', 'sbt', 'div', 'llt')


def programs() -> dict[str, Program]:
    out = [p.separable(name) for name in SEPARABLE]
    out += [p.separable(name, flags) for name in RANGED for flags in ('', 'x', 'u')]
    out += [p.pld(), p.sds(), p.sover_by_luma(False), p.sover_by_luma(True), pl.dlc(), pl.dlc(unbounded=True)]
    out += [pl.hsl(name) for name in ('hue', 'sat', 'col', 'lum')]
    out.append(pl.cli())
    return {prog.name: prog for prog in out}


# The programs beyond rule fetch.blend's 2 codes of the storage form on the host gate (tools/gpu/maxwell/rungcheck.py
# over one state of each program, image functions and attachment count; worst codes on 24B119 and 21H461): colour burn
# 59, divide 235, colour dodge 8, hue 3, colour 128, and every extended and unbounded form (dodge 70, divide 244, linear
# light 25, subtract 27, darker colour 34)
BEYOND_BOUND = frozenset(
    {'cbn', 'cbn.x', 'cbn.u', 'div', 'div.x', 'div.u', 'cdg', 'cdg.x', 'cdg.u', 'hue', 'col', 'dlc.u'}
    | {'llt.x', 'llt.u', 'sbt.x', 'sbt.u'}
)
# by QuartzCore build: 21H461's plus darker, 255 codes in every state
BEYOND_BOUND_ON = {'21H461': frozenset({'pdi'})}
# by image function: luminosity and saturation over 21H461's sar and ara images, 13 codes
BEYOND_WITH_IMAGE = {'sar': frozenset({'lum', 'sat'}), 'ara': frozenset({'lum', 'sat'})}
# by texture function: the colour matrices, whose source leaves the premultiplied range the unit's programs assume
# (rungcheck over one state of each texture function, program, memoryless image and attachment count, 24B119 and
# 21H461: up to 242 codes); hard light, linear light, lighten, pin light and the colour matrix stay within the bound,
# darker colour too but under c3b
_COLOUR_MATRIX_BEYOND = frozenset(
    {'cli', 'dif', 'drk', 'dso', 'lbn', 'lso', 'lum', 'mpy', 'ovl', 'pdi', 'pld', 'sat', 'sbt', 'sds', 'slt'}
)
BEYOND_WITH_TEXTURE = dict.fromkeys(('c33', 'c3s', 'c4b', 'c4p'), _COLOUR_MATRIX_BEYOND) | {
    'c3b': _COLOUR_MATRIX_BEYOND | {'dlc'}
}


def program_name(mode: str, extended: bool, unbounded: bool) -> str | None:
    """Give the program for blend function `mode` under the state's range flags, None where none is written."""
    if extended and unbounded:
        return None
    name = mode + ('.x' if extended else '.u' if unbounded else '')
    return name if name in PROGRAMS else None


def record(prog: Program) -> dict:
    """Give the program as packfile.iterated_bytes lays out a PackIteratedBlend."""
    source, dest = (prog.alpha[1], prog.alpha[2]) if prog.alpha[0] == 'ff' else ('ONE', 'ZERO')
    return {
        'name': prog.name,
        'words': prog.words,
        'enable': ENABLE_RGBA if prog.alpha[0] == 'ucode' else ENABLE_RGB,
        'constants': dict(prog.consts),
        'alpha_source': OGL[source],
        'alpha_dest': OGL[dest],
    }


PROGRAMS = programs()
