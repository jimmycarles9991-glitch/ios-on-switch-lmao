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


# The host gate (tools/gpu/maxwell/rungcheck.py over every state of the pack the exclusion covers, the source saturated
# as raw_fetch_free.h sets it; worst codes against the storage form, 24B119 and 21H461) puts these beyond rule
# fetch.blend's 2 codes on most of their texture functions: colour burn 255 (on 21 of 22 texture functions), divide 251
# (21 of 22), colour dodge 24 (10 of 22), hue 168 (15 of 22), colour 225 (9 of 22). Each also costs more passes than the
# interlocked storage form (raw_chip_facts.h kIteratedProven), so no draw would take its form. The extended forms blend
# into no 8-bit target (QuartzCore's extended range states are XR formats): dodge 70, divide 244, linear light 25,
# subtract 27 when an 8-bit unorm one was gated.
BEYOND_BOUND = frozenset({'cbn', 'cbn.x', 'div', 'div.x', 'cdg', 'cdg.x', 'hue', 'col', 'llt.x', 'sbt.x'})
# by QuartzCore build: 21H461's plus darker, 255 codes in every state (210 of 210)
BEYOND_BOUND_ON = {'21H461': frozenset({'pdi'})}
# by image function: luminosity and saturation over sar and ara images (16 of 38 states, 137 codes)
BEYOND_WITH_IMAGE = {'sar': frozenset({'lum', 'sat'}), 'ara': frozenset({'lum', 'sat'})}
# by texture function: the colour matrices, whose source leaves the premultiplied range the unit's programs assume
# (151 of 152 states beyond, up to 255 codes); hard light, linear light, lighten, pin light and the colour matrix stay
# within the bound, darker colour too but under c3b. Luminosity over the spl, dff and dfg functions' memoryless images,
# whose colour leaves the premultiplied domain (3 to 20 codes).
_COLOUR_MATRIX_BEYOND = frozenset(
    {'cli', 'dif', 'drk', 'dso', 'lbn', 'lso', 'lum', 'mpy', 'ovl', 'pdi', 'pld', 'sat', 'sbt', 'sds', 'slt'}
)
BEYOND_WITH_TEXTURE = dict.fromkeys(('c33', 'c3s', 'c4b', 'c4p'), _COLOUR_MATRIX_BEYOND) | {
    'c3b': _COLOUR_MATRIX_BEYOND | {'dlc'},
    'spl': frozenset({'lum'}),
    'dff': frozenset({'lum'}),
    'dfg': frozenset({'lum'}),
}
# On an 8-bit unorm target only, whose form saturates its source: the unbounded forms under 24B119's clr and dgd
# texture functions and dfr, rsd, scd and usd images (up to 255 codes; over src every one is within). On RGBA16Float,
# where the source is not saturated, every unbounded state is within (84 of 84 on 24B119, 20 of 20 on 21H461).
BEYOND_SATURATED_WITH_TEXTURE = {
    'clr': frozenset({'cbn.u', 'cdg.u', 'div.u'}),
    'dgd': frozenset({'cdg.u', 'div.u', 'dlc.u'}),
}
BEYOND_SATURATED_WITH_IMAGE = {
    'dfr': frozenset({'cbn.u', 'div.u'}),
    'rsd': frozenset({'div.u'}),
    'scd': frozenset({'cbn.u', 'div.u', 'dlc.u', 'sbt.u'}),
    'usd': frozenset({'div.u', 'sbt.u'}),
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
