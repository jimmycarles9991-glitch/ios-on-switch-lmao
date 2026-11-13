"""The fetch-free form of a Core Animation state that blends by fetching its colour (raw_pack.h PipelineFetchFreeForm).

QuartzCore keeps some blend functions in the fragment (Pipeline::Spec::lift_blend_function). The same state with its
fragment blend set to none writes the blend's source and fetches nothing; the ROP then does the blend:
  rung 2  the blend has a fixed-function equation (blend_function_info 0-25): the state's own pipeline blend of it
  rung 3  it has none: the iterated blend program iterated/table.py writes for it, or for the colour matrix (vcm) the
          one the GPU's macro loads with the matrix uniform of each draw
A state takes a form only where that is the same arithmetic as the fragment's blend. Not where the blend is one of a
shader's own reasons: an HDR scale (s) or EDR factor (e) applied around it, a destination function (D) on the colour,
or a texture function QuartzCore never lifts a blend out of (texture_function_info flag 3). Nor where the host gate
(tools/gpu/maxwell/rungcheck.py) has not shown the form within rule fetch.blend's bound of the storage form: a line
whose constants are not those this QuartzCore makes of its name (another build's journal), colour opcodes, the texture
functions whose programs discard every lane it tries, and the programs and image pairings beyond it
(iterated/table.py). Rung 2 also needs an
8-bit unorm target and no extended or unbounded range (the ROP clamps an 8-bit target's source, the fragment does
not). Rung 3 also needs the state to write its blended colour alone and that colour to be its first: the unit refuses
a draw of more than one colour target, so the draw narrows the targets to it.

The form is given as edits of the state's R line (free_tokens), which directpack.mm applies to make it.
"""

import os
import re
import sys

import caspec

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'iterated'))
import table

FIXED_FUNCTION_BLENDS = range(26)
UNCOMPARED_TEXTURE_FUNCTIONS = ('kfh', 'dkf')
# its program and constants come from each draw's uniform (fetchfree_link.py: the GPU's macro loads them)
COLOUR_MATRIX = 'vcm'
UNORM8 = (70, 80)  # MTLPixelFormatRGBA8Unorm, MTLPixelFormatBGRA8Unorm


def _image_reads_dest(tables: caspec.Tables, function: int) -> bool:
    return bool(tables.t['image_function_info'][function] >> 3 & 1)


def classify(tables: caspec.Tables, name: str, line: str) -> tuple[int, str]:
    """Give (rung, the iterated program for rung 3 or '') of a state's fetch-free form, else (0, why it has none)."""
    try:
        s = caspec.decode(tables, name)
    except (KeyError, IndexError, ValueError):
        return 0, 'name'
    colors = _colors(line)
    why = _foreign(tables, line) or _kept_in_fragment(tables, s) or _unproven(tables, s) or _target(s, colors)
    if why:
        return 0, why
    if s.fragB in FIXED_FUNCTION_BLENDS:
        return (0, 'extended range') if s.flag('x') or s.flag('u') else (2, '')
    return _iterated(tables, s, colors)


def _iterated(tables: caspec.Tables, s: caspec.Spec, colors: dict[int, tuple[int, int]]) -> tuple[int, str]:
    """Give (3, the iterated program) of a blend with no fixed-function equation, else (0, why it has none)."""
    mode = {v: k for k, v in tables.dec['BlendFunction'].items()}[s.fragB]
    matrix = mode == COLOUR_MATRIX and not (s.flag('x') or s.flag('u'))
    program = COLOUR_MATRIX if matrix else table.program_name(mode, s.flag('x'), s.flag('u'))
    if not program:
        return 0, f'no iterated program for {mode}'
    if program in table.BEYOND_BOUND | table.BEYOND_BOUND_ON.get(tables.t['build'], frozenset()):
        return 0, f'{program} beyond the bound on the host gate'
    names = _image_names(tables, s)
    if any(program in table.BEYOND_WITH_IMAGE.get(image, ()) for image in names):
        return 0, f'{program} beyond the bound on the host gate with its image function'
    texture = {v: k for k, v in tables.dec['TextureFunction'].items()}.get(s.T)
    if program in table.BEYOND_WITH_TEXTURE.get(texture, ()):
        return 0, f'{program} beyond the bound on the host gate with its texture function'
    if s.S != 0 or any(mask for c, (_fmt, mask) in colors.items() if c != s.S):
        return 0, 'writes another colour'
    return 3, program


def _kept_in_fragment(tables: caspec.Tables, s: caspec.Spec) -> str:
    """Give why the blend is the shader's own, '' where the ROP can do it."""
    if s.fragB == tables.none_blend:
        return 'no fragment blend'
    if tables.tfi_flags(s.T) >> 1 & 1 or any(_image_reads_dest(tables, i.I) for i in s.images[: _images(s)]):
        return 'reads the destination elsewhere'
    if tables.tfi_flags(s.T) >> 3 & 1:
        return 'texture function keeps its blend'
    if s.flag('s') or s.flag('e'):
        return 'HDR scale or EDR factor'
    return 'destination function' if s.D else ''


def _foreign(tables: caspec.Tables, line: str) -> str:
    """Give why the line is not this QuartzCore's state of its name, '' where it is."""
    return '' if caspec.made_of_name(tables, line) else "constants not this QuartzCore's of its name"


def _unproven(tables: caspec.Tables, s: caspec.Spec) -> str:
    if any(any(image.O) for image in s.images[: _images(s)]):
        return 'colour opcodes'
    texture = {v: k for k, v in tables.dec['TextureFunction'].items()}.get(s.T)
    return f'{texture} discards every lane on the host gate' if texture in UNCOMPARED_TEXTURE_FUNCTIONS else ''


def _image_names(tables: caspec.Tables, s: caspec.Spec) -> list[str]:
    names = {v: k for k, v in tables.dec['ImageFunction'].items()}
    return [names.get(image.I, '') for image in s.images[: _images(s)]]


def _target(s: caspec.Spec, colors: dict[int, tuple[int, int]]) -> str:
    return '' if s.S in colors and colors[s.S][0] in UNORM8 else 'blended colour not 8-bit unorm'


def free_tokens(tables: caspec.Tables, name: str, line: str, rung: int) -> str:
    """Give the edits that make the fetch-free form's R line of `line`, comma-separated.

    They set the fragment blend to none and the destination count to 0; on rung 2 they also give the blended colour's
    attachment field the blend in the pipeline, as QuartzCore's descriptor has it for the blend lifted.
    """
    s = caspec.decode(tables, name)
    edits = []
    for constant, value in (('fc_blend_function', tables.none_blend), ('fc_dest_count', 0)):
        m = re.search(rf'\b{constant}:(\d+):[0-9a-f]+', line)
        if not m:
            raise ValueError(f'{name}: no {constant}')
        edits.append(f'{constant}:{m[1]}:{value:02x}')
    if rung == 2:
        m = re.search(rf' c{s.S}=(\S+)', line)
        a = [int(x) for x in m[1].split(':')]
        lifted = caspec.decode(tables, name)
        lifted.pipeB = s.fragB
        caspec._rules(tables)._blend(tables.v27 or tables, lifted, a)
        edits.append(f'c{s.S}=' + ':'.join(map(str, a)))
    return ','.join(edits)


def tag(tables: caspec.Tables, name: str, line: str) -> tuple[str, str]:
    """Give a fetching state's " free=<rung>:<colour>:<program>;<edits>" tag ('' for none) and why it has none."""
    rung, found = classify(tables, name, line)
    if not rung:
        return '', found
    colour = caspec.decode(tables, name).S
    return f' free={rung}:{colour}:{found};{free_tokens(tables, name, line, rung)}', ''


def _images(s: caspec.Spec) -> int:
    return getattr(s, 'decoded', len(s.images))


def _colors(line: str) -> dict[int, tuple[int, int]]:
    """Give each colour attachment of an R line: (MTLPixelFormat, write mask)."""
    head = line.split(' | ', 1)[0]
    return {int(c): (int(f), int(m)) for c, f, m in re.findall(r' c(\d+)=(\d+):(\d+):', head)}
