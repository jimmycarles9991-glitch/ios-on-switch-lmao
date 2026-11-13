"""The fake device's guess at the iterated blend unit, read off clb197.h's field names.

Only a stand-in for the loop's tests, which the device's readback experiments exist to replace.

An instruction computes OPERATION over its products A*B and C*D (ADD and SUBTRACT over A and C alone), swizzles it,
writes the masked lanes to its PASS_OUTPUT temp, and NONE makes it the pass's result, which the next pass reads as PBR.
Alpha is source over destination (the common blend block blend.py sets), or with ALPHA_ENABLE the R lane of the last
R_TO_A pass.
"""

from dataclasses import dataclass, field

from tools.benchloop import blend

Rgb = tuple[float, float, float]
Colour = tuple[float, float, float, float]


@dataclass
class _State:
    src: Colour
    dst: Colour
    constants: list[Rgb]
    temps: list[Rgb] = field(default_factory=lambda: [(0.0, 0.0, 0.0)] * 3)
    pbr: Rgb = (0.0, 0.0, 0.0)


def _select(name: str, s: _State, constant: Rgb) -> Rgb:
    src, dst, temps, pbr = s.src, s.dst, s.temps, s.pbr
    table = {
        'ZERO': (0.0, 0.0, 0.0),
        'ONE': (1.0, 1.0, 1.0),
        'SRC_RGB': src[:3],
        'DEST_RGB': dst[:3],
        'SRC_AAA': (src[3],) * 3,
        'DEST_AAA': (dst[3],) * 3,
        'ONE_MINUS_SRC_AAA': (1 - src[3],) * 3,
        'ONE_MINUS_DEST_AAA': (1 - dst[3],) * 3,
        'TEMP0_RGB': temps[0],
        'TEMP1_RGB': temps[1],
        'TEMP2_RGB': temps[2],
        'PBR_RGB': pbr,
        'CONSTANT_RGB': constant,
    }
    return table.get(name, (0.0, 0.0, 0.0))


def _operate(op: str, a: Rgb, b: Rgb, c: Rgb, d: Rgb) -> Rgb:
    ab = tuple(x * y for x, y in zip(a, b, strict=True))
    cd = tuple(x * y for x, y in zip(c, d, strict=True))
    lanes = {
        'ADD_PRODUCTS': lambda i: ab[i] + cd[i],
        'SUB_PRODUCTS': lambda i: ab[i] - cd[i],
        'MIN': lambda i: min(ab[i], cd[i]),
        'MAX': lambda i: max(ab[i], cd[i]),
        'RCP': lambda i: 1 / ab[i] if ab[i] else float('inf'),
        'ADD': lambda i: a[i] + c[i],
        'SUBTRACT': lambda i: a[i] - c[i],
    }[op]
    return lanes(0), lanes(1), lanes(2)


def _swizzle(v: Rgb, how: str) -> Rgb:
    return {
        'RGB': v,
        'GBR': (v[1], v[2], v[0]),
        'RRR': (v[0],) * 3,
        'GGG': (v[1],) * 3,
        'BBB': (v[2],) * 3,
    }.get(how, v)


def _instruction(word: int, s: _State) -> Rgb:
    f = {k: str(v) for k, v in blend.field_values(word).items()}
    index = int(f['constant_input_select'][-1]) if f['constant_input_select'].startswith('CONSTANT') else 0
    constant = s.constants[index] if index < len(s.constants) else (0.0, 0.0, 0.0)
    a, b, c, d = (_select(f[k], s, constant) for k in OPERANDS)
    return _swizzle(_operate(f['operation'], a, b, c, d), f['output_swizzle'])


def apply(words: list[int], passes: int, alpha: bool, colours: tuple[Colour, Colour]) -> Colour:
    src, dst = colours
    s = _State(src, dst, [], pbr=dst[:3])
    per_pass = max(1, len(words) // max(1, passes))
    for p in range(passes):
        result = s.pbr
        for word in words[p * per_pass : (p + 1) * per_pass]:
            value = _instruction(word, s)
            out = str(blend.field_values(word)['pass_output'])
            if out == 'NONE':
                result = value
            elif out.startswith('TEMP'):
                s.temps[int(out[-1])] = value
        s.pbr = result
    to_alpha = [_instruction(w, s)[0] for w in words if blend.field_values(w)['output_swizzle'] == 'R_TO_A']
    a = to_alpha[-1] if alpha and to_alpha else src[3] + dst[3] * (1 - src[3])
    return s.pbr[0], s.pbr[1], s.pbr[2], a


OPERANDS = ('operand_a', 'operand_b', 'operand_c', 'operand_d')
