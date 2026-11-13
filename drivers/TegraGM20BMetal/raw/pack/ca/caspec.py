#!/usr/bin/env python3
"""Core Animation's render pipeline states from their names, as QuartzCore makes them (qc-<build>.json: qctables.py).

A port of CA::OGL::MetalContext's Pipeline::Spec::decode_name (the name's segments into the spec),
get_pipeline_descriptor (the colour attachments, the blend, the vertex descriptor), create_vertex_shader and
create_fragment_shader with FragmentShader::Spec::get_config (the functions' names and constants). state_fields(name)
gives the state's journal R line fields, as the frontend journals what CA made (NativeBridgeMetalFrontend.mm
journalRenderPipeline): what the pack generator probes.

usage: caspec.py <qc-<build>.json> <corpus.log>   checks every Core Animation R line of the corpus against its name
"""

import json
import re
import sys
from collections.abc import Callable
from types import ModuleType

NONE_BLEND = 58  # BlendFunction "non"
BOOL, UCHAR, UCHAR2, UCHAR4 = 53, 49, 50, 52  # MTLDataType
FLAG_BITS = {'g': 2, 'n': 3, 'h': 4, 'f': 5, 'x': 6, 'u': 7, 'p': 8, 'e': 9, 's': 10, 'c': 11, 'l': 12, 'r': 13}
# CA::OGL::non_srgb_pixel_format
NON_SRGB = {11: 10, 31: 30, 71: 70, 81: 80, 551: 550, 553: 552, 555: 554}
# image functions create_fragment_shader gives no LOD bias (its 0x10840007fe00 mask)
NO_LOD_BIAS = {9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 34, 39, 44}

Const = tuple[str, int, str]


class Tables:
    def __init__(self, path: str) -> None:
        with open(path) as f:
            self.t = json.load(f)
        self.dec = self.t['decoders']
        self.tfi = [bytes.fromhex(x) for x in self.t['texture_function_info']]
        self.none_blend = self.dec['BlendFunction']['non']
        self.v27 = None
        if self.t.get('layout') == 27:
            import caspec27  # noqa: PLC0415  (it builds on this module's segments and constants)

            self.v27 = caspec27.Tables27(self.t)

    def tfi_flags(self, tf: int) -> int:  # texture_function_info[tf] bytes 5-6
        return self.v27.tfi_flags(tf) if self.v27 else self.tfi[tf][5] | self.tfi[tf][6] << 8

    def shader_type(self, tf: int) -> int:
        return self.v27.shader_type(tf) if self.v27 else self.tfi[tf][0] & 0x3F

    def images(self, tf: int) -> int:
        return self.t['tex_image_count'][tf]


class Image:
    def __init__(self) -> None:
        self.I = self.C = self.F = self.N = self.q = 0
        self.M = -1
        self.O = [0, 0, 0, 0]


class Spec:
    def __init__(self) -> None:
        self.images = [Image(), Image()]
        self.T = self.D = self.L = self.S = self.flags = self.P = 0
        self.fragB = self.pipeB = NONE_BLEND
        self.A = 1
        self.pflags: set[str] = set()

    def flag(self, letter: str) -> bool:
        return bool(self.flags >> FLAG_BITS[letter] & 1)


class _Segment:
    """CA::OGL::MetalContext::decode_segment over one NameDecoder table: its letters up to '_' or the name's end."""

    def __init__(
        self, tab: list[dict], decoders: dict[str, dict[str, int]], field: Callable[[str, int, int], None]
    ) -> None:
        self.letters = {row['letter']: row for row in tab}
        self.decoders = decoders
        self.field = field
        for row in tab:
            if row['letter'].isupper() and not row['flag'] and row['count'] >= 1:
                for j in range(row['count']):
                    field(row['letter'], j, row['default'])

    def value(self, row: dict, text: str) -> int:
        d = self.decoders.get(row['letter'])
        if d is not None and text in d:
            return d[text]
        number = re.match(r'[+-]?\d+', text)
        return int(number.group(0)) if number else row['default']

    def read(self, name: str, at: int, flag: Callable[[str], None]) -> int:
        """Decode from `at`; return where the next segment begins."""

        def lower(i: int) -> bool:
            return i < len(name) and not name[i].isupper() and name[i] != '_'

        while at < len(name) and name[at] != '_':
            row = self.letters.get(name[at])
            at += 1
            if row is not None and row['flag']:
                while lower(at):
                    if name[at] in self.letters:
                        flag(name[at])
                    at += 1
            elif row is not None:
                for j in range(row['count']):
                    self.field(row['letter'], j, self.value(row, name[at : at + row['width']]))
                    at += row['width']
            while lower(at):
                at += 1
        return at + 1 if at < len(name) and name[at] == '_' else at


def decode(tables: Tables, name: str) -> Spec:
    """Pipeline::Spec::decode_name."""
    if tables.v27:
        import caspec27  # noqa: PLC0415

        return caspec27.decode(tables.v27, name)
    s = Spec()
    dec, nt = tables.dec, tables.t['name_tables']

    def pipe_field(c: str, _j: int, v: int) -> None:
        setattr(s, 'P' if c == 'P' else 'pipeB', v)

    def frag_field(c: str, _j: int, v: int) -> None:
        setattr(s, 'fragB' if c == 'B' else c, v)

    def frag_flag(letter: str) -> None:
        s.flags |= 1 << FLAG_BITS[letter]

    pdec = {'P': dec['MTLPixelFormat'], 'B': dec['BlendFunction']}
    at = _Segment(nt['pipeline'], pdec, pipe_field).read(name, 0, s.pflags.add)
    fdec = {
        'T': dec['TextureFunction'],
        'B': dec['BlendFunction'],
        'D': dec['DestinationFunction'],
        'L': dec['VertexLayout'],
    }
    at = _Segment(nt['fragment'], fdec, frag_field).read(name, at, frag_flag)
    idec = {'I': dec['ImageFunction'], 'C': dec['CoordFunction'], 'F': dec['TextureFilter'], 'O': dec['Opcode']}
    for k in range(tables.images(s.T)):
        img = s.images[k] if k < 2 else Image()

        def image_field(c: str, j: int, v: int, img: Image = img) -> None:
            if c == 'O':
                img.O[j] = v
            else:
                setattr(img, c, v)

        def image_flag(_letter: str, img: Image = img) -> None:
            img.q = 1

        at = _Segment(nt['image'], idec, image_field).read(name, at, image_flag)
    return s


def _const(name: str, dtype: int, value: int) -> Const:
    # at the type's size, as the device journals a constant and the raw runtime keys a state by it (constantBytes)
    width = {UCHAR2: 2, UCHAR4: 4}.get(dtype, 1)
    return name, dtype, (value & ((1 << 8 * width) - 1)).to_bytes(width, 'little').hex()


def _texcoords(tables: Tables, s: Spec) -> list[int]:
    """Pipeline::decode_name's vertex shader spec: each image's texture coordinate count."""
    out = [0, 0]
    if not s.flag('g'):
        for k in range(min(2, tables.images(s.T))):
            out[k] = tables.t['image_texcoord_count'][s.images[k].I] & 3
    return out


def vertex_function(tables: Tables, s: Spec) -> tuple[str, list[Const]]:
    """create_vertex_shader: the vertex function's name and constants."""
    g, h = s.flag('g'), s.flag('h')
    vtype = 0 if g else tables.t['fragment_shader_type_info'][tables.shader_type(s.T)][1] & 0x1F
    named = tables.t['vertex_shader_type_info'][vtype]
    if named:
        return named + ('_lph' if h else '_lpf'), []
    name = tables.t['fixed_vert_names'][0 if h else 1][1 if g else 0]
    consts = [_const('fc_generic', BOOL, g), _const('fc_vertex_layout', UCHAR, 0 if g else s.L)]
    if g:
        return name, [*consts, _const('fc_base_color', BOOL, 0)]
    tc = _texcoords(tables, s)
    return name, [*consts, _const('fc_texcoord_count0', UCHAR, tc[0]), _const('fc_texcoord_count1', UCHAR, tc[1])]


def _reads_dest(tables: Tables, s: Spec, images: list[Image]) -> int:
    ifi = tables.t['image_function_info']
    return int(s.fragB != NONE_BLEND or bool(tables.tfi_flags(s.T) >> 1 & 1) or any(ifi[i.I] >> 3 & 1 for i in images))


def _shader_constants(tables: Tables, s: Spec, images: list[Image]) -> list[Const]:
    """Give the constants create_fragment_shader sets before the per-image ones."""
    t, g, flags = tables.t, s.flag('g'), tables.tfi_flags(s.T)
    ifi, tfilt = t['image_function_info'], t['texture_filter_info']
    noise = int(g or s.flag('n'))
    lod = 1 if g else 0 if flags >> 5 & 1 else int(any(i.I not in NO_LOD_BIAS for i in images))
    gamma = g or s.flag('e') or flags >> 7 & 1 or any(o in (11, 12) for i in images for o in i.O)
    c = [
        _const('fc_generic', BOOL, g),
        _const('fc_vertex_layout', UCHAR, s.L),
        _const('fc_framebuffer_fetch', BOOL, s.flag('f')),
        _const('fc_attachment_count', UCHAR, s.A),
        _const('fc_has_lod_bias', BOOL, lod),
        _const('fc_has_noise_scale', BOOL, noise),
        _const('fc_has_ycbcr_matrix', BOOL, g or any(ifi[i.I] >> 1 & 1 for i in images)),
        _const('fc_has_gamma_luts', BOOL, bool(gamma)),
        _const('fc_has_asg_77_weights', BOOL, g or any(tfilt[i.F] >> 1 & 1 for i in images)),
        _const('fc_has_noise_tex', BOOL, noise),
        _const('fc_has_color_params', BOOL, g or any(any(i.O) for i in images)),
        _const('fc_has_lut_tex', BOOL, g or bool(flags >> 9 & 1)),
    ]
    reads = _reads_dest(tables, s, images)
    for k in range(3):
        has = 0 if s.flag('f') else int(k < s.A) if g else int(s.S <= k < s.S + reads)
        c.append(_const(f'fc_has_attachment_{k}', BOOL, has))
    if g:
        return c
    return [
        *c,
        _const('fc_texture_function', UCHAR, s.T),
        _const('fc_blend_function', UCHAR, s.fragB),
        _const('fc_destination_function', UCHAR, s.D),
        _const('fc_image_count', UCHAR, len(images)),
        _const('fc_dest_index', UCHAR, s.S),
        _const('fc_dest_count', UCHAR, reads),
        _const('fc_noise_width', UCHAR, 32 if s.flag('n') else 0),
        *(_const(n, BOOL, s.flag(f)) for n, f in _BOOL_FLAGS),
    ]


_BOOL_FLAGS = (
    ('fc_clamp_color', 'l'),
    ('fc_extended_range', 'x'),
    ('fc_unbounded_range', 'u'),
    ('fc_debug_xr', 'r'),
    ('fc_has_premultiply', 'p'),
    ('fc_has_hdr_scale', 's'),
    ('fc_has_edr_factor', 'e'),
)


def _image_constants(tables: Tables, s: Spec, k: int, count: int) -> list[Const]:
    t, g = tables.t, s.flag('g')
    img = s.images[k] if k < count else Image()
    c = []
    if not g:
        here = k < count
        ops = sum((o & 0x1F) << 8 * j for j, o in enumerate(img.O))
        c += [
            _const(f'fc_image_function{k}', UCHAR, img.I),
            _const(f'fc_coord_function{k}', UCHAR, img.C),
            _const(f'fc_plane_count{k}', UCHAR, t['image_plane_count'][img.I] if here else 0),
            _const(f'fc_texcoord_count{k}', UCHAR, t['image_texcoord_count'][img.I] if here else 0),
            _const(f'fc_memoryless_index{k}', UCHAR, img.M + 1),
            _const(f'fc_filter{k}', UCHAR, img.F),
            _const(f'fc_color_inputs{k}', UCHAR, img.N),
            _const(f'fc_color_opcodes{k}', UCHAR4, ops),
            _const(f'fc_color_opaque{k}', BOOL, img.q),
        ]
    c.append(_const(f'fc_has_color_trc{k}', BOOL, g or 7 in img.O))
    c.append(_const(f'fc_has_color_cube{k}', BOOL, g or any(o in (8, 9, 10) for o in img.O)))
    for j, letter in enumerate('ABC'):
        has = g or (k < count and j < t['image_plane_count'][img.I] and img.M < 0)
        c.append(_const(f'fc_has_image_{k}{letter}', BOOL, bool(has)))
    return c


def fragment_function(tables: Tables, s: Spec) -> tuple[str, list[Const]]:
    """create_fragment_shader with get_config: the fragment function's name and constants."""
    h = s.flag('h')
    named = tables.t['fragment_shader_type_info'][tables.shader_type(s.T)][0]
    if named:
        return named + ('_lph' if h else '_lpf'), []
    name = tables.t['fixed_frag_names'][0 if h else 1][1 if s.flag('c') else 0]
    if name is None:
        raise ValueError('no fixed fragment function of that precision')
    count = tables.images(s.T)
    c = _shader_constants(tables, s, s.images[:count])
    for k in range(2):
        c += _image_constants(tables, s, k, count)
    return name, c


def _attachment_format(s: Spec, i: int) -> int:
    if i == 0:
        return s.P
    if 'w' in s.pflags and i == 2:
        return 55
    p = s.P
    # BGR10_XR (554): RGB10A8_2P_XR10, RGBA16Float on a virtualized device (CADeviceIsVirtualized)
    fmt = (110 if p in (90, 94) else 550 if p == 554 else p) if p > 0x59 else {10: 30, 43: 80, 63: 113}.get(p, p)
    return NON_SRGB.get(fmt, fmt)


def _blend(tables: Tables, s: Spec, a: list[int]) -> None:
    info = tables.t['blend_function_info'][s.pipeB]
    op, src, dst = info >> 5 & 7, info >> 8 & 0xF, info >> 12
    a[2], a[8] = 1, op
    if s.pipeB >= 24:
        a[1], a[6], a[7] = 1, src, dst
        return
    a[3], a[4], a[5] = src, dst, op
    a[6] = (1 if s.pipeB < 22 else 0) if s.pipeB > 18 else src
    a[7] = (5 if s.pipeB <= 21 else 1) if s.pipeB > 18 else dst


def _function_targets(s: Spec, atts: list[list[int]]) -> bool:
    """Set the targets of the texture functions with their own (texture_function_info bit 4).

    Returns whether the vertex descriptor stays.
    """
    while len(atts) < 3:
        atts.append([0, 15, 0, 1, 0, 0, 1, 0, 0])
    tf = s.T
    if 0 <= tf - 0x4F < 8:
        for k in (0, 1):
            atts[k][1:3] = [0, 0]
        atts[2][1:6] = [8, 1, 1, 1, 4 if tf - 0x4F > 3 else 0]
        return False
    if 0 <= tf - 0x58 < 2:
        atts[2][1:3] = [8, 0]
        atts[1 if s.S == 0 else 0][1:3] = [0, 0]
        return False
    if tf == 0x57:
        for k in (0, 1, 2):
            atts[k][1:3] = [8 if k == 2 else 0, 0]
    return True


def color_attachments(tables: Tables, s: Spec) -> tuple[list[list[int]], bool]:
    """Give get_pipeline_descriptor's colour attachments and whether it keeps the vertex descriptor.

    Each: [format, writeMask, blending, srcRGB, dstRGB, rgbOp, srcA, dstA, aOp].
    """
    atts = [[_attachment_format(s, i), 15 if (s.flag('g') or i == s.S) else 0, 0, 1, 0, 0, 1, 0, 0] for i in range(s.A)]
    if s.pipeB != NONE_BLEND and len(atts) > s.S:
        _blend(tables, s, atts[s.S])
    vertex = True
    if tables.tfi_flags(s.T) >> 4 & 1:
        vertex = _function_targets(s, atts)
        atts = [a for a in atts if a[0]]
    return atts, vertex


def vertex_attributes(tables: Tables, s: Spec) -> list[tuple[int, int, int, int]]:
    """Give the vertex descriptor's attributes (index, format, offset, stride); none for a generic state."""
    if s.flag('g'):
        return []
    info = tables.t['vertex_layout_info'][s.L]
    stride, count = info[0] & 0x3F, info[0] >> 6 & 7
    return sorted((a & 7, a >> 3 & 0x1F, a >> 8 & 0x3F, stride) for a in info[1 : 1 + count])


def state_line(tables: Tables, name: str) -> str:
    """Give the journal's R line of the state `name`, spaced as the frontend journals it (the probe keys by that text).

    Each function's part is its name, a blank, its constants: two blanks before the next '|' for one without constants,
    as RawMetalLibrary.mm raw_metal_pipeline_key makes the key the pack is looked up by.
    """
    s = decode(tables, name)
    rules = _rules(tables)
    atts, vertex = rules.color_attachments(tables.v27 or tables, s)
    parts = ['samples=1', 'topo=0', 'ds=0/0']
    parts += [f'c{i}=' + ':'.join(map(str, a)) for i, a in enumerate(atts)]
    if vertex:
        attributes = rules.vertex_attributes(tables.v27 or tables, s)
        parts += [f'a{i}={fmt}:{off}:1:{stride}:1:1' for i, fmt, off, stride in attributes]

    def text(fn: tuple[str, list[Const]]) -> str:
        return fn[0] + ' ' + ' '.join(f'{k}:{t}:{v}' for k, t, v in sorted(fn[1]))

    v = text(rules.vertex_function(tables.v27 or tables, s))
    f = text(rules.fragment_function(tables.v27 or tables, s))
    return f'R ok=1 label=com.apple.coreanimation.{name} ' + ' '.join(parts) + f' | v {v} | f {f} | '


def _rules(tables: Tables) -> ModuleType:
    """Give the module that makes a state's descriptor and functions as this QuartzCore does."""
    if tables.v27:
        import caspec27  # noqa: PLC0415

        return caspec27
    return sys.modules[__name__]


def state_fields(tables: Tables, name: str) -> str:
    """Give the R line's fields (after `label=...`) of the state `name`, spaces normalised: corpus_fields' form."""
    return corpus_fields(state_line(tables, name))[1]


def made_of_name(tables: Tables, line: str) -> bool:
    """Whether a Core Animation line's function constants are those this QuartzCore makes of its name.

    Another build's journal names states this one makes with other constants (a memoryless index past the pass's
    attachments, another blend function code). A name these tables do not decode, or a line that is not Core
    Animation's, is not judged: True.
    """
    name, fields = corpus_fields(line)
    if not name:
        return True
    try:
        made = state_fields(tables, name)
    except (KeyError, IndexError, ValueError):
        return True
    return _constants(made) == _constants(fields)


def _constants(fields: str) -> list[str]:
    return sorted(t for t in fields.split() if t.startswith('fc_'))


def corpus_fields(line: str) -> tuple[str | None, str | None]:
    """Give a Core Animation corpus R line's name and its fields as state_fields gives them."""
    m = re.match(r'R ok=\d+ label=com\.apple\.coreanimation\.(\S+) (.*)$', line)
    if not m:
        return None, None
    rest = re.sub(r' (lib|os)=\S+', '', ' ' + m.group(2))  # os=: the OS build the line was journaled under
    return m.group(1), re.sub(r'( \|)+$', '', ' '.join(rest.split()))


def main() -> None:
    tables = Tables(sys.argv[1])
    ok = bad = 0
    with open(sys.argv[2]) as f:
        for line in f:
            name, want = corpus_fields(line)
            if not name:
                continue
            got = state_fields(tables, name)
            ok += got == want
            bad += got != want
            if got != want and bad <= 6:
                gw, ww = got.split(), want.split()
                print(f'{name}:\n  got  {[x for x in gw if x not in ww][:12]}')
                print(f'  want {[x for x in ww if x not in gw][:12]}')
    print(f'{ok} of {ok + bad} Core Animation states made from their names alone')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
