#!/usr/bin/env python3
"""Extract Core Animation's pipeline-name grammar and the tables behind its pipeline descriptors from QuartzCore.

QuartzCore names every render pipeline it makes (CA::OGL::MetalContext::Pipeline::Spec::name) and can make one back
from its name (Pipeline::Spec::decode_name, the offline compilation manifest's load_pipeline). NSConstantDictionary
decoders and NameDecoder tables decode the name's segments; static tables (texture_function_info, blend_function_info,
...) give the descriptor and the function constants. The tables are data in the dylib; caspec.py ports the code that
reads them. Addresses are per build (most of the tables have no symbol): a new IPSW needs its own entry here.

usage: qctables.py <QuartzCore Mach-O, extracted from the dyld shared cache with its slide applied> <build> <out.json>
                   <QuartzCore.framework/default.metallib>
       (ipsw dyld extract <dyld_shared_cache_arm64> QuartzCore --slide -o <dir>)
The metallib's SHA-256 goes into the tables: the pack generator uses them only with that library (rawpack.py).
"""

import hashlib
import json
import struct
import sys

ADDRESSES = {
    '21H461': {
        'decoders': {
            'MTLPixelFormat': 0x1E6968A48,
            'BlendFunction': 0x1E6968908,
            'TextureFunction': 0x1E69688E0,
            'DestinationFunction': 0x1E6968930,
            'VertexLayout': 0x1E6968958,
            'ImageFunction': 0x1E6968980,
            'CoordFunction': 0x1E69689A8,
            'TextureFilter': 0x1E69689D0,
            'Opcode': 0x1E69689F8,
            'VertexShaderType': 0x1E6968A20,
        },
        'name_tables': {'pipeline': (0x1E6934698, 6), 'fragment': (0x1E6932A98, 19), 'image': (0x1E6933150, 8)},
        'texture_function_info': (0x1897163A8, 100, 7),
        'tex_image_count': (0x1897216F8, 100),
        'blend_function_info': (0x189716664, 59),
        'image_function_info': (0x18971673E, 45),
        'texture_filter_info': (0x18971676B, 12),
        'image_plane_count': (0x18972175C, 45),
        'image_texcoord_count': (0x189718674, 45),
        'vertex_layout_info': (0x189716778, 3),
        'fragment_shader_type_info': (0x1E69321B0, 57),
        'vertex_shader_type_info': (0x1E6934868, 19),
        'fixed_frag_names': 0x1E6934848,
        'fixed_vert_names': 0x1E6934A30,
        'manifest': (0x1E692CA10, 560),
    }
}


class Image:
    def __init__(self, path: str) -> None:
        with open(path, 'rb') as f:
            self.b = f.read()
        ncmds = struct.unpack_from('<I', self.b, 16)[0]
        off, self.segs = 32, []
        for _ in range(ncmds):
            cmd, size = struct.unpack_from('<II', self.b, off)
            if cmd == 0x19:  # LC_SEGMENT_64
                vm, _vsz, fo, fsz = struct.unpack_from('<QQQQ', self.b, off + 24)
                self.segs.append((vm, fo, fsz))
            off += size

    def read(self, va: int, n: int) -> bytes:
        for vm, fo, fsz in self.segs:
            if vm <= va < vm + fsz:
                return self.b[fo + va - vm : fo + va - vm + n]
        raise KeyError(hex(va))

    def ptr(self, va: int) -> int:
        return struct.unpack('<Q', self.read(va, 8))[0] & 0xFFFFFFFFFFFF

    def cstr(self, va: int) -> str:
        out = b''
        while b'\0' not in out:
            out += self.read(va + len(out), 64)
        return out[: out.index(b'\0')].decode('latin1')

    def cfstring(self, va: int) -> str | None:  # NSConstantString: isa, flags, bytes, length
        return (
            self.read(self.ptr(va + 16), struct.unpack('<Q', self.read(va + 24, 8))[0]).decode('latin1') if va else None
        )

    # NSConstantDictionary: isa, options, count, keys, objects (NSConstantIntegerNumber: isa, encoding, value)
    def nsdict(self, va: int) -> dict[str, int]:
        count, keys, objects = struct.unpack('<QQQ', self.read(va + 16, 24))
        keys &= 0xFFFFFFFFFFFF
        objects &= 0xFFFFFFFFFFFF
        return {
            self.cfstring(self.ptr(keys + 8 * k)): struct.unpack('<q', self.read(self.ptr(objects + 8 * k) + 16, 8))[0]
            for k in range(count)
        }


def name_table(img: Image, va: int, n: int) -> list[dict]:
    rows = []
    for k in range(n):
        e = img.read(va + 0x38 * k, 0x38)
        width, count, default = struct.unpack_from('<iii', e, 4)
        rows.append({'letter': chr(e[0]), 'flag': bool(e[1]), 'width': width, 'count': count, 'default': default})
    return rows


def main() -> None:
    path, build, out, metallib = sys.argv[1:5]
    a = ADDRESSES[build]
    img = Image(path)
    with open(metallib, 'rb') as f:
        digest = hashlib.sha256(f.read()).hexdigest()
    t = {'build': build, 'metallib_sha256': digest, 'decoders': {k: img.nsdict(v) for k, v in a['decoders'].items()}}
    t['name_tables'] = {k: name_table(img, va, n) for k, (va, n) in a['name_tables'].items()}
    va, n, w = a['texture_function_info']
    t['texture_function_info'] = [img.read(va + w * i, w).hex() for i in range(n)]
    for key in (
        'tex_image_count',
        'image_function_info',
        'texture_filter_info',
        'image_plane_count',
        'image_texcoord_count',
    ):
        va, n = a[key]
        t[key] = list(img.read(va, n))
    va, n = a['blend_function_info']
    t['blend_function_info'] = list(struct.unpack(f'<{n}H', img.read(va, 2 * n)))
    va, n = a['vertex_layout_info']
    t['vertex_layout_info'] = [list(struct.unpack('<5Q', img.read(va + 40 * i, 40))) for i in range(n)]
    va, n = a['fragment_shader_type_info']
    t['fragment_shader_type_info'] = [
        [img.cfstring(img.ptr(va + 16 * i)), img.read(va + 16 * i + 8, 1)[0]] for i in range(n)
    ]
    va, n = a['vertex_shader_type_info']
    t['vertex_shader_type_info'] = [img.cfstring(img.ptr(va + 24 * i + 8)) for i in range(n)]
    for key in ('fixed_frag_names', 'fixed_vert_names'):  # [!h][c] and [!h][g]
        t[key] = [[img.cfstring(img.ptr(a[key] + 16 * i + 8 * j)) for j in range(2)] for i in range(2)]
    va, n = a['manifest']
    t['manifest'] = []
    for i in range(n):
        e = va + 0x28 * i
        has_min, has_max = img.read(e + 8, 1)[0], img.read(e + 24, 1)[0]
        fmin, fmax = struct.unpack('<Q', img.read(e + 16, 8))[0], struct.unpack('<Q', img.read(e + 32, 8))[0]
        t['manifest'].append([img.cstr(img.ptr(e)), fmin if has_min else 0, fmax if has_max else 0])
    with open(out, 'w') as f:
        json.dump(t, f, indent=2, sort_keys=True)  # dprint's JSON layout (//:format_test)
        f.write('\n')


if __name__ == '__main__':
    main()
