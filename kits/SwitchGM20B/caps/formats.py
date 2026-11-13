"""A formats.tsv row joined with what the hardware tables say about its pipe format.

NIL's row (through NIL's own Format class, nil_format_table_gen.py), the texture header's word 0 packed as NIL's
descriptor code packs it, and the block size from Mesa's u_format.yaml.
"""

import importlib
import re
import sys
from dataclasses import dataclass

import yaml
from caps import CapsError, Headers, Row

# Every texel or block the copy engine moves whole: SET_REMAP_COMPONENTS takes 1-4 components of 1-4 bytes.
CE_BYTES = {size * count for size in range(1, 5) for count in range(1, 5)}
FLAGS = ['texture', 'render', 'blend', 'storage', 'zeta', 'filter', 'twod', 'copy', 'vic', 'dc', 'srgb', 'buffer']


def nil_format_class(nil_dir: str) -> type:
    """Return Mesa NIL's Format class, which reads one nil_formats.csv row."""
    sys.path.insert(0, nil_dir)
    return importlib.import_module('nil_format_table_gen').Format


@dataclass
class Sources:
    nil: dict
    format_class: type
    headers: Headers
    blocks: dict
    render_samples: str


def block_info(yaml_path: str) -> dict[str, tuple[int, int, int]]:
    with open(yaml_path, encoding='utf-8') as f:
        entries = yaml.safe_load(f)
    out = {}
    for e in entries:
        channels = e.get('channels') or (e.get('little_endian') or {}).get('channels') or []
        bits = sum(int(re.sub(r'\D', '', c) or 0) for c in channels)
        block = e.get('block', {})
        out[e['name']] = (bits // 8, block.get('width', 1), block.get('height', 1))
    return out


def tic_word0(fmt: object, headers: Headers) -> int:
    def put(field: str, value_name: str) -> int:
        hi, lo = headers.field('NVB197_TEXHEAD_BL_' + field)
        return (headers.value(value_name) & ((1 << (hi - lo + 1)) - 1)) << lo

    word = put('COMPONENTS', fmt.tcs())
    for i, c in enumerate('RGBA'):
        word |= put(f'{c}_DATA_TYPE', fmt.type(i))
    for i, c in enumerate('XYZW'):
        word |= put(f'{c}_SOURCE', fmt.src(i))
    return word


def sample_mask(counts: str) -> int:
    mask = 0
    for n in counts.split(','):
        mask |= int(n)
    return mask


def support_flags(fmt: object, pipe: str) -> list[str]:
    support = set(fmt._support)
    letters = {'T': 'texture', 'C': 'render', 'A': 'blend', 'S': 'storage', 'Z': 'zeta', 'B': 'buffer'}
    flags = [name for letter, name in letters.items() if letter in support]
    if 'T' in support and not all(t in 'UI' for t in fmt._types):
        flags.append('filter')
    if 'SRGB' in pipe:
        flags.append('srgb')
    return flags


def derive(row: Row, src: Sources) -> dict:
    c = row.cols
    out = {
        'mtl': int(c['mtl']),
        'name': c['name'],
        'pipe': c['pipe'],
        'grade': c['grade'],
        'flags': [name for name in ('vic', 'dc') if c[name] != '-'],
        'ct': 0,
        'zt': 0,
        'tic0': 0,
        'bytes': 0,
        'block_w': 1,
        'block_h': 1,
        'msaa': 1,
        'kind': int(c['kind'], 16) if c['kind'] != '-' else 0,
    }
    if c['pipe'] == '-':
        return out
    line = src.nil.get(c['pipe'])
    if line is None:
        raise CapsError(f'{row.where()}: {c["pipe"]} has no row in nil_formats.csv')
    fmt = src.format_class(line)
    flags = out['flags'] + support_flags(fmt, c['pipe'])
    if 'render' in flags or 'zeta' in flags:
        out['zt' if 'zeta' in flags else 'ct'] = src.headers.value(fmt.czt())
        out['msaa'] = sample_mask(src.render_samples)
        if 'render' in flags and 'NV902D_SET_DST_FORMAT_V_' + fmt._czt in src.headers.values:
            flags.append('twod')
    if 'texture' in flags:
        out['tic0'] = tic_word0(fmt, src.headers)
    nbytes, bw, bh = src.blocks.get(c['pipe'], (0, 1, 1))
    out.update(bytes=nbytes, block_w=bw, block_h=bh)
    if nbytes in CE_BYTES:
        flags.append('copy')
    out['flags'] = sorted(set(flags), key=FLAGS.index)
    return out
