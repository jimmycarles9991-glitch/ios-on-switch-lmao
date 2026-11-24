"""QuartzCore 27's Pipeline::Spec::name: a spec (caspec27.Spec) back into its name, as decode_name reads it.

Each segment writes its table's fields in order (pipeline encode_segment 24B119 0x18441175c, the fragment's and an
image's alike): the pipeline's format and an image's function always, any other field whose value is not the table's
default, each as its letter and its code (the decoder's, else the number at the field's width), a multi-value field
when any value differs, every value; then X and the set flags in the table's order. Names carry every image segment
the texture function samples.
"""

from caspec27 import Image, Spec, Tables27

PIPELINE_FLAGS = ('b', 'm', 'w')


def _code(decoder: dict[str, int] | None, value: int, width: int) -> str:
    if decoder is not None:
        for code, v in decoder.items():
            if v == value:
                return code
    return f'{value:0{width}d}'


def _segment(
    table: list[dict], decoders: dict[str, dict[str, int]], values: dict[str, list[int]], flags: set, always: str
) -> str:
    out = ''
    for row in table:
        letter = row['letter']
        if row['flag']:
            set_flags = [r['letter'] for r in table if r['letter'].islower() and r['letter'] in flags]
            if set_flags:
                out += letter + ''.join(set_flags)
            continue
        if letter.islower() or not row['count']:
            continue
        got = values.get(letter, [row['default']] * row['count'])
        if letter != always and all(v == row['default'] for v in got):
            continue
        out += letter + ''.join(_code(decoders.get(letter), v, row['width']) for v in got)
    return out


def encode(tables: Tables27, s: Spec) -> str:
    """Give the name of spec `s`."""
    dec, nt = tables.dec, tables.t['name_tables']
    pipeline = _segment(
        nt['pipeline'],
        {'P': dec['MTLPixelFormat'], 'B': dec['BlendFunction']},
        {'P': [s.P], 'B': [s.pipeB]},
        set(s.pflags),
        'P',
    )
    fragment = _segment(
        nt['fragment'],
        {
            'T': dec['TextureFunction'],
            'B': dec['BlendFunction'],
            'D': dec['DestinationFunction'],
            'L': dec['VertexLayout'],
        },
        {'T': [s.T], 'B': [s.fragB], 'D': [s.D], 'A': [s.A], 'S': [s.S], 'L': [s.L]},
        set(s.flags),
        '',
    )
    images = []
    for k in range(tables.images(s.T)):
        img = s.images[k] if k < len(s.images) else Image()
        images.append(
            _segment(
                nt['image'],
                {'I': dec['ImageFunction'], 'C': dec['CoordFunction'], 'F': dec['TextureFilter'], 'O': dec['Opcode']},
                {'I': [img.I], 'C': [img.C], 'F': [img.F], 'M': [img.M], 'N': [img.N], 'O': list(img.O)},
                {'q'} if img.q else set(),
                'I',
            )
        )
    return '_'.join([pipeline, fragment, *images])
