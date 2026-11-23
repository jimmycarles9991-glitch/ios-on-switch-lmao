"""The fetch-free form of a RenderBox state that blends by fetching its colour (pack.h PipelineFetchFreeForm).

RenderBox's composite() writes mix(D, blend(S, D), c): its blend mode of the source S and the fetched destination D,
taken by the pixel's coverage c (the primitive's edge, times the coverage target where the state reads it). The same
state with its blend and its destination read cleared from the constant block takes composite's other arm, which writes
c S and fetches nothing; the ROP then blends c S with D. That is the same colour wherever blend(c S, D) =
D + c (blend(S, D) - D) for every c in [0, 1]: the modes whose blend is D at a source of 0 and scales with the source
(and its alpha) beyond that. They are
  rung 2  a fixed-function equation: the form's pipeline blend (FIXED)
  rung 3  Core Animation's separable modes, the same formulas in exact arithmetic: the iterated blend program
          iterated/table.py writes for them (ITERATED)
copy (a coverage lerp: its factor is c, which c S does not carry) needs a second fragment output for dual-source blend,
and the modes that saturate the sum's alpha (plus_saturated, plus_darker) differ from mix() wherever c < 1; they keep
the storage form, as do custom blend functions, the modes of a colour's luma, and pin light (its select at S = Sa/2
takes the other arm than Core Animation's program, which differs wherever D > Da).
A form also needs a target the ROP blends (8-bit unorm, or RGBA16Float, where neither side clamps), a source the mode
does not saturate on a float target, a destination with its own alpha, and only the coverage target among the colours it
reads, which it must not write. Rung 3 also needs the blended colour to be the first and the only one written (the unit
refuses a draw of more than one colour target). A function whose tests follow its program (exports_depth, discards)
draws beside a depth or stencil attachment through its tests=1..4 variant records, to which the run-time links no
fetch-free form (pipelines.cpp variantPipelines), so such a state is left on the storage form.

The form is given as edits of the state's R line (fetch_free.h), which directpack.mm applies to make it under the
state's own key.
"""

from collections.abc import Callable
from pathlib import Path

import rbspec

UNORM8 = (70, 80)  # MTLPixelFormatRGBA8Unorm, MTLPixelFormatBGRA8Unorm
RGBA16_FLOAT = 115

# MTLBlendFactor and MTLBlendOperation
ZERO, ONE, ONE_MINUS_SOURCE_COLOR, ONE_MINUS_SOURCE_ALPHA = 0, 1, 3, 5
ONE_MINUS_DESTINATION_COLOR, DESTINATION_ALPHA, ONE_MINUS_DESTINATION_ALPHA = 7, 8, 9
ADD, REVERSE_SUBTRACT = 0, 2

# mode: (source RGB, destination RGB, RGB operation, source alpha, destination alpha, alpha operation)
FIXED = {
    'normal': (ONE, ONE_MINUS_SOURCE_ALPHA, ADD, ONE, ONE_MINUS_SOURCE_ALPHA, ADD),
    'source_atop': (DESTINATION_ALPHA, ONE_MINUS_SOURCE_ALPHA, ADD, DESTINATION_ALPHA, ONE_MINUS_SOURCE_ALPHA, ADD),
    'destination_over': (ONE_MINUS_DESTINATION_ALPHA, ONE, ADD, ONE_MINUS_DESTINATION_ALPHA, ONE, ADD),
    'destination_out': (ZERO, ONE_MINUS_SOURCE_ALPHA, ADD, ZERO, ONE_MINUS_SOURCE_ALPHA, ADD),
    'xor': (
        ONE_MINUS_DESTINATION_ALPHA,
        ONE_MINUS_SOURCE_ALPHA,
        ADD,
        ONE_MINUS_DESTINATION_ALPHA,
        ONE_MINUS_SOURCE_ALPHA,
        ADD,
    ),
    'plus': (ONE, ONE, ADD, ONE, ONE, ADD),
    'screen': (ONE, ONE_MINUS_SOURCE_COLOR, ADD, ONE, ONE_MINUS_SOURCE_ALPHA, ADD),
    'plus_over': (ONE, ONE, ADD, ONE, ONE_MINUS_SOURCE_ALPHA, ADD),
    'exclusion': (ONE_MINUS_DESTINATION_COLOR, ONE_MINUS_SOURCE_COLOR, ADD, ONE, ONE_MINUS_SOURCE_ALPHA, ADD),
    'reverse_subtract': (ONE, ONE, REVERSE_SUBTRACT, ONE, ONE, REVERSE_SUBTRACT),
}
ITERATED = {
    'multiply': 'mpy',
    'overlay': 'ovl',
    'darken': 'drk',
    'lighten': 'ltn',
    'hard_light': 'hlt',
    'difference': 'dif',
    'linear_burn': 'lbn',
}
KEPT = {
    'copy': 'copy: its coverage lerp needs dual-source blend',
    'plus_saturated': 'its saturated alpha is not linear in coverage',
    'plus_darker': 'its saturated alpha is not linear in coverage',
    'pin_light': 'pin light: at S = Sa/2 it takes the min arm and pin the max, apart where D > Da',
}


def classify(tables: rbspec.Tables, line: str) -> tuple[int, str]:
    """Give (rung, the iterated program for rung 3 or '') of a state's fetch-free form, else (0, why it has none).

    (0, '') for a state that does not fetch its colour to blend it.
    """
    spec = rbspec.decode(line)
    if spec is None or spec.function not in tables.functions or not spec.field(tables, 'reads_dest'):
        return 0, ''
    for refusal in (_blend, _tests, _reads, _target, _single_target):
        why = refusal(tables, spec)
        if why:
            return 0, why
    mode = tables.modes[spec.field(tables, 'mode')]
    return (2, '') if mode in FIXED else (3, ITERATED[mode])


def _blend(tables: rbspec.Tables, spec: rbspec.Spec) -> str:
    """Give why the blend stays in the fragment, '' where the ROP can do it."""
    if not spec.field(tables, 'blends'):
        return 'reads the destination outside a blend'
    if spec.field(tables, 'custom_blend'):
        return 'custom blend function'
    number = spec.field(tables, 'mode')
    mode = tables.modes.get(number)
    if mode is None:
        return f'mode {number} not read'
    if mode not in FIXED and mode not in ITERATED:
        return KEPT.get(mode, f'{mode} is not linear in coverage')
    return ''


def _tests(tables: rbspec.Tables, spec: rbspec.Spec) -> str:
    """Give why a form would not draw as the storage form does around the depth and stencil tests, '' where it does."""
    if spec.field(tables, 'discards_transparent'):
        return 'discards a transparent source'
    if tables.functions[spec.function] != 'early_tests' and spec.tested:
        return 'draws through its tests=1..4 variants'
    return ''


def _reads(tables: rbspec.Tables, spec: rbspec.Spec) -> str:
    """Give why the form would still fetch a colour it writes, '' where it reads none."""
    target = spec.colours.get(tables.coverage_target)
    if spec.field(tables, 'reads_coverage') and target and target[1]:
        return 'reads the coverage target it writes'
    return ''


def _target(tables: rbspec.Tables, spec: rbspec.Spec) -> str:
    """Give why the blended colour's target takes no form, '' where it takes one."""
    colour = spec.field(tables, 'colour')
    fmt = spec.colours.get(colour, (0, 0, ''))[0]
    if fmt not in UNORM8 and fmt != RGBA16_FLOAT:
        return f'blended colour {fmt} not RGBA8, BGRA8 or RGBA16Float'
    if spec.field(tables, 'alpha_only'):
        return 'destination alpha only'
    saturates = spec.field(tables, 'mode') in tables.saturated_unless_extended
    if fmt == RGBA16_FLOAT and saturates and not spec.field(tables, 'extended'):
        return 'saturates a source alpha the float target does not clamp'
    return ''


def _single_target(tables: rbspec.Tables, spec: rbspec.Spec) -> str:
    """Give why the iterated blend unit cannot draw the form, '' where it can: it blends the first target alone."""
    if tables.modes[spec.field(tables, 'mode')] in FIXED:
        return ''
    colour = spec.field(tables, 'colour')
    if colour:
        return 'blended colour not the first'
    if any(mask for slot, (_fmt, mask, _rest) in spec.colours.items() if slot != colour):
        return 'writes another colour'
    return ''


def free_tokens(tables: rbspec.Tables, line: str, rung: int) -> str:
    """Give the edits that make the fetch-free form's R line of `line`, comma-separated.

    The fragment's constant block with the blend and the destination read cleared, and on rung 2 the blended colour's
    attachment with its blend.
    """
    spec = rbspec.decode(line)
    edits = [f'f/#0:36:{rbspec.edited(spec, tables)}']
    if rung == 2:
        colour = spec.field(tables, 'colour')
        fmt, mask, _rest = spec.colours[colour]
        factors = FIXED[tables.modes[spec.field(tables, 'mode')]]
        edits.append(f'c{colour}={fmt}:{mask}:1:' + ':'.join(map(str, factors)))
    return ','.join(edits)


def tag(tables: rbspec.Tables, line: str) -> tuple[str, str]:
    """Give a fetching state's " rung=<n> free=<rung>:<colour>:<program>;<edits>" tag ('' for none) and why it has none.

    The reason is '' for a state that does not fetch its colour to blend it.
    """
    rung, found = classify(tables, line)
    if not rung:
        return '', found
    colour = rbspec.decode(line).field(tables, 'colour')
    return f' rung={rung} free={rung}:{colour}:{found};{free_tokens(tables, line, rung)}', ''


def tagger(directory: Path, quartzcore_digest: str) -> tuple[Callable[[str], str], dict[str, int]] | None:
    """Give a function tagging the IPSW's RenderBox lines with their forms, and the counts it fills.

    The counts are of its forms by rung and of why the others have none. None where rb/ has no tables for the IPSW
    whose QuartzCore library has that digest.
    """
    tables = rbspec.for_quartzcore(directory, quartzcore_digest)
    if tables is None:
        return None
    why: dict[str, int] = {}

    def tagged(line: str) -> str:
        if not rbspec.renderbox_line(line):
            return line
        free, reason = tag(tables, line)
        key = reason or (f'form on rung {free[6]}' if free else '')
        if key:
            why[key] = why.get(key, 0) + 1
        return line.replace(' samples=', free + ' samples=', 1) if free else line

    return tagged, why
