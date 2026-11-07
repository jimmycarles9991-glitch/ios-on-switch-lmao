#!/usr/bin/env python3
"""programs.bin for nvdec from NVK driver records under m2v cache dirs.

Per nvk_shader: 'NVPG', code_size, cbuf count, data_size, the cbuf types, the 80-byte shader header (the hdr NVK
uploads before the code on pre-Turing), the code, then NAK's shader data (the constant buffer of type shader_data; NVK
uploads it after the code, 256-aligned).
usage: mkprograms.py <out programs.bin> <cache dir>...
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nvkshader

TYPES = {'invalid': 0, 'root': 1, 'shader_data': 2, 'desc_set': 3, 'dynamic_ubo': 4, 'ubo_desc': 5}


def main() -> None:
    out, seen = bytearray(), set()
    for d in sys.argv[2:]:
        for root, _, files in os.walk(d):
            for f in files:
                p = os.path.join(root, f)
                with open(p, 'rb') as record:
                    b = record.read()
                if b[:8] != b'M2VPPLC\0':
                    continue
                for _t, _k, data in nvkshader.records(b):
                    s = nvkshader.shader(data)
                    if not s:
                        continue
                    key = s['hdr'][:80] + s['code'] + s['data']
                    if key in seen:
                        continue
                    seen.add(key)
                    types = bytes(TYPES.get(c[0], c[0]) if isinstance(c[0], str) else c[0] for c in s['cbufs'])
                    out += b'NVPG' + struct.pack('<III', len(s['code']), len(types), len(s['data'])) + types
                    out += s['hdr'][:80] + s['code'] + s['data']
    with open(sys.argv[1], 'wb') as programs:
        programs.write(out)
    print(f'{sys.argv[1]}: {len(seen)} programs')


if __name__ == '__main__':
    main()
