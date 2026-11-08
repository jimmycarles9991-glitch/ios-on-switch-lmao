"""iOS 27's QuartzCore pipeline-name tables, as qctables.py extracts them for its own builds.

QuartzCore 27 keeps the grammar and most tables but lays them out anew: texture_function_info rows are 8 bytes (flags
at 6-7), blend_function_info rows 3 bytes (flags, then the operation and factors), image_function_info 2 bytes, the
vertex shader table 24-byte rows, NameDecoder rows 0x48 bytes (a decoder getter and a setter per value after the
default), and its offline compilation manifest holds compute specs (K...) beside the render pipelines. Most tables
have symbols here; the addresses are this build's.
"""

import struct
import sys
from pathlib import Path
from typing import Protocol

sys.path.insert(0, str(Path(__file__).resolve().parents[5] / 'tools/gpu/shaders'))
import metallib  # the shader crawl's reader of Metal libraries

ADDRESSES = {
    '24B119': {
        'decoders': {
            'MTLPixelFormat': 0x1E2289C18,
            'BlendFunction': 0x1E2289AD8,
            'TextureFunction': 0x1E2289AB0,
            'DestinationFunction': 0x1E2289B00,
            'VertexLayout': 0x1E2289B28,
            'ImageFunction': 0x1E2289B50,
            'CoordFunction': 0x1E2289B78,
            'TextureFilter': 0x1E2289BA0,
            'Opcode': 0x1E2289BC8,
            'VertexShaderType': 0x1E2289BF0,
        },
        'name_tables': {'pipeline': (0x1E225C780, 6), 'fragment': (0x1E225BD50, 20), 'image': (0x1E225C2F8, 8)},
        'texture_function_info': (0x1846EAEDC, 136),
        'tex_image_count': (0x1846F6310, 136),
        'blend_function_info': (0x1846EB31C, 64),
        'image_function_info': (0x1846EB3E4, 48),
        'texture_filter_info': (0x1846EB444, 12),
        'image_plane_count': (0x1846F6398, 48),
        'image_texcoord_count': (0x1846FF918, 48),
        'vertex_layout_info': (0x1846EB450, 3),
        'fragment_shader_type_info': (0x1E05D59A0, 78),
        'vertex_shader_type_info': (0x1E05D5EB0, 18),
        'fixed_frag_names': 0x1E05DA7C0,
        'fixed_vert_names': 0x1E05DA7E0,
        'manifest': (0x1E05CFAC8, 607),
    }
}


class Image(Protocol):
    def read(self, va: int, n: int) -> bytes: ...
    def ptr(self, va: int) -> int: ...
    def cstr(self, va: int) -> str: ...
    def cfstring(self, va: int) -> str | None: ...
    def nsdict(self, va: int) -> dict[str, int]: ...


def _name_table(img: Image, va: int, n: int) -> list[dict]:
    rows = []
    for k in range(n):
        e = img.read(va + 0x48 * k, 0x48)
        width, count, default = struct.unpack_from('<iii', e, 4)
        rows.append({'letter': chr(e[0]), 'flag': bool(e[1]), 'width': width, 'count': count, 'default': default})
    return rows


def _names(img: Image, va: int, n: int) -> list[str | None]:
    return [img.cfstring(img.ptr(va + 8 * i)) if img.ptr(va + 8 * i) else None for i in range(n)]


def _manifest(img: Image, va: int, n: int) -> list[list]:
    out = []
    for i in range(n):
        e = va + 0x28 * i
        has_min, has_max = img.read(e + 8, 1)[0], img.read(e + 24, 1)[0]
        fmin, fmax = struct.unpack('<Q', img.read(e + 16, 8))[0], struct.unpack('<Q', img.read(e + 32, 8))[0]
        out.append([img.cstr(img.ptr(e)), fmin if has_min else 0, fmax if has_max else 0])
    return out


def functions(library: bytes) -> dict[str, list[list]]:
    """Give each function of QuartzCore's library with its declared constants: [name, MTLDataType, required]."""
    return {f.name: [[c.name, c.type, c.required] for c in f.constants] for f in metallib.functions(library) or []}


def tables(img: Image, a: dict) -> dict:
    """Give a QuartzCore 27's tables (qctables.py adds the build, the metallib's digest and its functions)."""
    t: dict = {'layout': 27, 'decoders': {k: img.nsdict(v) for k, v in a['decoders'].items()}}
    t['name_tables'] = {k: _name_table(img, va, n) for k, (va, n) in a['name_tables'].items()}
    va, n = a['texture_function_info']
    t['texture_function_info'] = [img.read(va + 8 * i, 8).hex() for i in range(n)]
    for key in ('tex_image_count', 'texture_filter_info', 'image_plane_count', 'image_texcoord_count'):
        va, n = a[key]
        t[key] = list(img.read(va, n))
    va, n = a['image_function_info']
    t['image_function_info'] = list(struct.unpack(f'<{n}H', img.read(va, 2 * n)))
    va, n = a['blend_function_info']
    t['blend_function_info'] = [img.read(va + 3 * i, 3).hex() for i in range(n)]
    va, n = a['vertex_layout_info']
    t['vertex_layout_info'] = [list(struct.unpack('<5Q', img.read(va + 40 * i, 40))) for i in range(n)]
    va, n = a['fragment_shader_type_info']
    t['fragment_shader_type_info'] = [
        [img.cfstring(img.ptr(va + 16 * i)) if img.ptr(va + 16 * i) else None, img.read(va + 16 * i + 8, 1)[0]]
        for i in range(n)
    ]
    va, n = a['vertex_shader_type_info']
    t['vertex_shader_type_info'] = [
        img.cfstring(img.ptr(va + 24 * i + 8)) if img.ptr(va + 24 * i + 8) else None for i in range(n)
    ]
    t['fixed_frag_names'] = _names(img, a['fixed_frag_names'], 2)  # [c]: cph, cpf
    t['fixed_vert_names'] = _names(img, a['fixed_vert_names'], 2)  # [g]: spc, gen
    va, n = a['manifest']
    t['manifest'] = _manifest(img, va, n)
    return t
