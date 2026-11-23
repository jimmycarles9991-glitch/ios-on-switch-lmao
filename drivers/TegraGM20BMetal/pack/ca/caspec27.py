"""iOS 27's Core Animation render pipeline states from their names (tables with "layout": 27, qctables27.py).

A port of QuartzCore 27's CA::OGL::MetalContext Pipeline::Spec::decode_name, get_pipeline_descriptor,
create_vertex_shader and create_fragment_shader with FragmentShader::Spec::create_function_constants and get_config.
The grammar is 21H461's (caspec.py) with an image's six colour opcodes, a perspective flag (v) and a 64-entry blend
table. What changed: every named or fixed function is the half-precision one (_lph); the second and third colour
attachments are RGBA16Float, or R32Float for the third under the pipeline's w flag; the fragment function no longer
reads its attachments (fc_has_attachment_<k> are always false) but takes fc_perspective, and the vertex function takes
it too.
"""

from caspec import BOOL, UCHAR, UCHAR2, UCHAR4, Const, _const, _Segment

RGBA16_FLOAT, R32_FLOAT = 115, 55
# fc_has_lod_bias: image functions with no LOD bias (create_function_constants' 0x420_00fffe00 mask)
NO_LOD_BIAS = {*range(9, 24), 37, 42}
# fc_has_color_cube: the opcodes that read a colour cube (mask 0x4807 over opcode - 8)
CUBE_OPCODES = {8, 9, 10, 19, 22}
# Texture functions that draw to their own targets (texture_function_info flag 4): the path coverage functions
PATH_COVERAGE = range(0x77, 0x7B)
PATH_BLIT = (0x7C, 0x7D)
PATH_CLEAR = 0x7B
# create_function_constants counts two images for this texture function wherever it has fewer
TWO_IMAGE_FUNCTION = 0x3D
# Texture functions QuartzCore picks among by a filter's parameters at draw time, each family drawn with the same
# pipeline: glass by its refraction, chromatic and edge amounts (GlassBackgroundFilter::render), the blur chain's passes
# by radius (create_blur_surface_internal). The offline manifest holds only the members Apple's GPUs warm.
VARIANT_FAMILIES = (
    ('gba', 'gbb', 'gbc', 'gbd', 'gbe', 'gbf', 'gbg', 'gbh'),
    ('gsa', 'gsb', 'gsc', 'gsd', 'gse', 'gsf', 'gsg', 'gsh'),
    ('gfg', 'gfs'),
    ('ds4', 'ds6', 'ds8', 'db2', 'db4', 'nb7', 'n11', 'n15', 'n19', 'n23', 'n27', 'sb3'),
)


class Image:
    def __init__(self) -> None:
        self.I = self.C = self.F = self.N = self.q = 0
        self.M = -1
        self.O = [0] * 6


class Spec:
    def __init__(self, none_blend: int) -> None:
        self.images = [Image(), Image()]
        self.decoded = 0
        self.T = self.D = self.L = self.S = self.P = 0
        self.fragB = self.pipeB = none_blend
        self.A = 1
        self.flags: set[str] = set()
        self.pflags: set[str] = set()

    def flag(self, letter: str) -> bool:
        return letter in self.flags


class Tables27:
    """The parts of a QuartzCore 27's tables the decoding reads."""

    def __init__(self, t: dict) -> None:
        self.t = t
        self.dec = t['decoders']
        self.none = self.dec['BlendFunction']['non']
        self.tfi = [bytes.fromhex(x) for x in t['texture_function_info']]
        self.bfi = [bytes.fromhex(x) for x in t['blend_function_info']]

    def tfi_flags(self, tf: int) -> int:  # texture_function_info[tf] bytes 6-7
        return self.tfi[tf][6] | self.tfi[tf][7] << 8

    def shader_type(self, tf: int) -> int:
        return self.tfi[tf][0] & 0x7F

    def images(self, tf: int) -> int:
        return self.t['tex_image_count'][tf]

    def image_flags(self, function: int) -> int:  # image_function_info[function] byte 0
        return self.t['image_function_info'][function] & 0xFF


def decode(tables: Tables27, name: str) -> Spec:
    """Pipeline::Spec::decode_name (FragmentShader::Spec::decode_name for the fragment and image segments)."""
    s = Spec(tables.none)
    dec, nt = tables.dec, tables.t['name_tables']

    def pipe_field(c: str, _j: int, v: int) -> None:
        setattr(s, 'P' if c == 'P' else 'pipeB', v)

    def frag_field(c: str, _j: int, v: int) -> None:
        setattr(s, 'fragB' if c == 'B' else c, v)

    pdec = {'P': dec['MTLPixelFormat'], 'B': dec['BlendFunction']}
    at = _Segment(nt['pipeline'], pdec, pipe_field).read(name, 0, s.pflags.add)
    fdec = {
        'T': dec['TextureFunction'],
        'B': dec['BlendFunction'],
        'D': dec['DestinationFunction'],
        'L': dec['VertexLayout'],
    }
    at = _Segment(nt['fragment'], fdec, frag_field).read(name, at, s.flags.add)
    idec = {'I': dec['ImageFunction'], 'C': dec['CoordFunction'], 'F': dec['TextureFilter'], 'O': dec['Opcode']}
    s.decoded = min(2, tables.images(s.T))
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
    for img in s.images[s.decoded :]:
        img.M = 0  # an image no segment decoded keeps the spec's zeroed bytes
    return s


def vertex_function(tables: Tables27, s: Spec) -> tuple[str, list[Const]]:
    """create_vertex_shader over the vertex spec decode_name derives from the fragment's."""
    g = s.flag('g')
    vtype = 0 if g else tables.t['fragment_shader_type_info'][tables.shader_type(s.T)][1] & 0x1F
    named = tables.t['vertex_shader_type_info'][vtype]
    if named:
        return named + '_lph', []
    name = tables.t['fixed_vert_names'][1 if g else 0]
    consts = [_const('fc_generic', BOOL, g), _const('fc_vertex_layout', UCHAR, 0 if g else s.L)]
    if g:
        return name, [*consts, _const('fc_base_color', BOOL, 0)]
    tc = [tables.t['image_texcoord_count'][img.I] & 3 for img in s.images[: s.decoded]] + [0, 0]
    return name, [
        *consts,
        _const('fc_perspective', BOOL, s.flag('v')),
        _const('fc_texcoord_count0', UCHAR, tc[0]),
        _const('fc_texcoord_count1', UCHAR, tc[1]),
    ]


def _reads_dest(tables: Tables27, s: Spec) -> int:
    images = s.images[: s.decoded]
    return int(
        s.fragB != tables.none
        or bool(tables.tfi_flags(s.T) >> 1 & 1)
        or any(tables.image_flags(i.I) >> 3 & 1 for i in images)
    )


def _shader_constants(tables: Tables27, s: Spec) -> list[Const]:
    t, g, flags, images = tables.t, s.flag('g'), tables.tfi_flags(s.T), s.images[: s.decoded]
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
        _const('fc_has_ycbcr_matrix', BOOL, g or any(tables.image_flags(i.I) >> 1 & 1 for i in images)),
        _const('fc_has_gamma_luts', BOOL, bool(gamma)),
        _const('fc_has_asg_77_weights', BOOL, g or any(t['texture_filter_info'][i.F] >> 1 & 1 for i in images)),
        _const('fc_has_noise_tex', BOOL, noise),
        _const('fc_has_color_params', BOOL, g or any(any(i.O) for i in images)),
        _const('fc_has_lut_tex', BOOL, g or bool(flags >> 9 & 1)),
        *(_const(f'fc_has_attachment_{k}', BOOL, 0) for k in range(3)),
    ]
    if g:
        return c
    return [
        *c,
        _const('fc_texture_function', UCHAR, s.T),
        _const('fc_blend_function', UCHAR, s.fragB),
        _const('fc_destination_function', UCHAR, s.D),
        _const('fc_image_count', UCHAR, tables.images(s.T)),
        _const('fc_dest_index', UCHAR, s.S),
        _const('fc_dest_count', UCHAR, _reads_dest(tables, s)),
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
    ('fc_perspective', 'v'),
)


def _has_image(tables: Tables27, s: Spec, k: int, plane: int) -> bool:
    if s.flag('g'):
        return True
    count = tables.images(s.T)
    shown = 2 if s.T == TWO_IMAGE_FUNCTION else count
    if k >= shown or plane >= tables.t['image_plane_count'][s.images[k].I]:
        return False
    return k >= count or s.images[k].M < 0


def _image_constants(tables: Tables27, s: Spec, k: int) -> list[Const]:
    t, g = tables.t, s.flag('g')
    here = k < s.decoded
    img = s.images[k] if here else Image()
    c = []
    if not g:
        c += [
            _const(f'fc_image_function{k}', UCHAR, img.I),
            _const(f'fc_coord_function{k}', UCHAR, img.C),
            _const(f'fc_plane_count{k}', UCHAR, t['image_plane_count'][img.I] if here else 0),
            _const(f'fc_texcoord_count{k}', UCHAR, t['image_texcoord_count'][img.I] if here else 0),
            _const(f'fc_memoryless_index{k}', UCHAR, img.M + 1),
            _const(f'fc_filter{k}', UCHAR, img.F),
            _const(f'fc_color_inputs{k}', UCHAR, img.N),
            _const(f'fc_color_opcodes{k}', UCHAR4, sum((o & 0x1F) << 8 * j for j, o in enumerate(img.O[:4]))),
            _const(f'fc_color_opcodes_ext{k}', UCHAR2, (img.O[4] & 0x1F) | (img.O[5] & 0x1F) << 8),
            _const(f'fc_color_opaque{k}', BOOL, img.q),
        ]
    ops = s.images[k].O
    c.append(_const(f'fc_has_color_trc{k}', BOOL, g or 7 in ops))
    c.append(_const(f'fc_has_color_cube{k}', BOOL, g or any(o in CUBE_OPCODES for o in ops)))
    c += [_const(f'fc_has_image_{k}{letter}', BOOL, _has_image(tables, s, k, j)) for j, letter in enumerate('ABC')]
    return c


def fragment_function(tables: Tables27, s: Spec) -> tuple[str, list[Const]]:
    """create_fragment_shader with create_function_constants and get_config."""
    named = tables.t['fragment_shader_type_info'][tables.shader_type(s.T)][0]
    if named:
        return named + '_lph', []
    name = tables.t['fixed_frag_names'][1 if s.flag('c') else 0]
    return name, _shader_constants(tables, s) + _image_constants(tables, s, 0) + _image_constants(tables, s, 1)


def _blend(tables: Tables27, s: Spec, a: list[int]) -> None:
    info = tables.bfi[s.pipeB]
    op, src, dst = info[1] & 7, info[1] >> 3 & 0xF, info[2] & 0xF
    a[2], a[8] = 1, op
    if s.pipeB >= 24:
        a[1], a[6], a[7] = 1, src, dst
        return
    a[3], a[4], a[5] = src, dst, op
    a[6] = (1 if s.pipeB < 22 else 0) if s.pipeB > 18 else src
    a[7] = (5 if s.pipeB < 22 else 1) if s.pipeB > 18 else dst


def _function_targets(s: Spec, atts: list[list[int]]) -> None:
    """Set the targets of the texture functions with their own (texture_function_info flag 4)."""
    while len(atts) < 3:
        atts.append([0, 15, 0, 1, 0, 0, 1, 0, 0])
    if s.T in PATH_COVERAGE:
        for k in (0, 1):
            atts[k][1:3] = [0, 0]
        atts[2][1:6] = [8, 1, 1, 1, 0]
    elif s.T in PATH_BLIT:
        atts[2][1:3] = [8, 0]
        atts[1 if s.S == 0 else 0][1:3] = [0, 0]
    else:
        for k in (0, 1, 2):
            atts[k][1:3] = [8 if k == 2 else 0, 0]


def color_attachments(tables: Tables27, s: Spec) -> tuple[list[list[int]], bool]:
    """Give get_pipeline_descriptor's colour attachments and whether it keeps the vertex descriptor."""
    w = 'w' in s.pflags
    atts = [
        [
            s.P if i == 0 else R32_FLOAT if (i == 2 and w) else RGBA16_FLOAT,
            15 if (s.flag('g') or i == s.S) else 0,
            0,
            1,
            0,
            0,
            1,
            0,
            0,
        ]
        for i in range(s.A)
    ]
    if s.pipeB != tables.none and len(atts) > s.S:
        _blend(tables, s, atts[s.S])
    if not tables.tfi_flags(s.T) >> 4 & 1:
        return atts, True
    if s.T not in (*PATH_COVERAGE, *PATH_BLIT, PATH_CLEAR):
        return atts, True
    _function_targets(s, atts)
    return [a for a in atts if a[0]], False


def vertex_attributes(tables: Tables27, s: Spec) -> list[tuple[int, int, int, int]]:
    """Give the vertex descriptor's attributes (index, format, offset, stride); none for a generic state."""
    if s.flag('g'):
        return []
    info = tables.t['vertex_layout_info'][s.L]
    stride, count = info[0] & 0x3F, info[0] >> 6 & 7
    return sorted((a & 7, a >> 3 & 0x1F, a >> 8 & 0x3F, stride) for a in info[1 : 1 + count])


def variants(tables: Tables27, name: str) -> list[str]:
    """Give the names of the states `name`'s texture function family draws with the same pipeline (VARIANT_FAMILIES).

    Each member keeps the name's other segments, with as many image segments as it samples (the first one repeated).
    """
    prefix, _, fragment = name.partition('_')
    head, *images = fragment.split('_')
    if not head.startswith('T') or not images:
        return []
    family = next((f for f in VARIANT_FAMILIES if head[1:4] in f), ())
    out = []
    for code in family:
        count = tables.images(tables.dec['TextureFunction'][code])
        segments = (images + [images[0]] * count)[:count]
        out.append('_'.join([prefix, 'T' + code + head[4:], *segments]))
    return [n for n in out if n != name]


def lifted(tables: Tables27, fragment: str, fmt: int, blend: int) -> bool:
    """Pipeline::Spec::lift_blend_function: whether `blend` of `fragment` goes to the pipeline (hardware) blend.

    A fragment that writes its first attachment or a float target keeps more blends; one destination blend (61, add
    without extended range) is lifted as add.
    """
    s = decode(tables, 'P0000Xm_' + fragment)
    if tables.tfi_flags(s.T) >> 3 & 1:
        return False
    if blend in (3, tables.none):
        return True
    narrow = s.S != 0 or fmt in (RGBA16_FLOAT, 125)
    if not s.flag('s') and blend == 61 and not s.flag('x') and not narrow:
        blend = 12
    info = tables.bfi[blend][0]
    if (narrow and info >> 5 & 1) or blend > 25 or (blend == 12 and (narrow or s.flag('s'))):
        return False
    if (s.flag('s') and info >> 4 & 1) or blend == 0:
        return False
    return s.D == 0 or not info & 1
