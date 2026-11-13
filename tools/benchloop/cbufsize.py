"""cbufsize: whether the chip honours a constant buffer's bound size that is not a multiple of 256.

Each cell reads one word c[1][offset] into all four channels of an RGBA32F texel, with the shader data (word i is the
float 1000 + i) bound at exactly `dataBound` bytes (CB_SELECTOR_A's size; the bench's records bypass the
runtime's 256-byte rounding, raw_cbuf.h). NVK binds every buffer at a 256-byte multiple; the split twin's frontier was
bound at 16, 32, 64, 480 or 672 and read wrong below its size until rounded. On T-v4c these cells ran through the
rounding, so they read the zeroed bytes past the data, not the chip's bound.
"""

import struct

from tools.benchloop import protocol
from tools.benchloop.sm53 import encode, operands, sph
from tools.benchloop.sm53.encode import Cbuf

SIZES = (16, 32, 64, 128, 255, 256, 257, 480, 512, 672, 768)
FIXED = (0x0, 0x3C, 0x100, 0x1DC, 0x29C)
DATA_BYTES = 1024
FIRST = 1000.0


def offsets(size: int) -> list[int]:
    """Give the fixed probes plus the last word wholly inside the bound, the word across it, and the first past it."""
    last, across, past = (size - 4) & ~3, (size - 1) & ~3, (size + 3) & ~3
    return sorted({o for o in (*FIXED, last, across, past) if 0 <= o <= DATA_BYTES - 4})


def program(offset: int, bound: int) -> dict:
    body = [encode.mov(r, Cbuf(1, offset)).sched(delay=1) for r in range(3)]
    body.append(encode.mov(3, Cbuf(1, offset)).sched(delay=15))
    code = encode.bundle(encode.finish(body))
    return {
        'sph': protocol.b64(sph.pixel(operands.attributes(code))),
        'code': protocol.b64(code),
        'data': protocol.b64(b''.join(struct.pack('<f', FIRST + i) for i in range(DATA_BYTES // 4))),
        'gprs': 8,
        'dataBound': bound,
    }


def generate() -> list[dict]:
    return [
        {
            'id': f'cbuf-{size}-at{offset:03x}',
            'kind': 'readback',
            'target': {'format': 'rgba32float', 'width': 8, 'height': 2},
            'program': program(offset, size),
            'readback': 'full',
            'clear': [0.125] * 4,
            'meta': {'family': 'cbufsize', 'bound': size, 'offset': offset},
        }
        for size in SIZES
        for offset in offsets(size)
    ]


def read_word(result: dict) -> float | None:
    data = protocol.unb64((result.get('texels') or {}).get('data') or '')
    return struct.unpack('<f', data[:4])[0] if len(data) >= 4 else None


def analyse(experiments: list[dict], results: dict[str, dict]) -> dict:
    """Give per bound size the offsets that read their word, read 0 or read something else, and a verdict per size.

    'exact': every word wholly inside the bound reads itself and every word past it reads 0; `across` lists the word
    that straddles a bound off a 4-byte step, either reading allowed.
    """
    sizes: dict[int, dict] = {}
    for e in experiments:
        bound, offset = e['meta']['bound'], e['meta']['offset']
        r = results.get(e['id']) or {}
        row = sizes.setdefault(bound, {'word': [], 'zero': [], 'other': [], 'failed': []})
        got = read_word(r) if r.get('status') == 'ok' else None
        if got is None:
            row['failed'].append([offset, r.get('status', 'missing')])
        elif got == FIRST + offset // 4:
            row['word'].append(offset)
        elif got == 0.0:
            row['zero'].append(offset)
        else:
            row['other'].append([offset, got])
    for bound, row in sizes.items():
        word_outside = [o for o in row['word'] if o >= bound]
        zero_inside = [o for o in row['zero'] if o + 4 <= bound]
        clean = not (word_outside or zero_inside or row['other'] or row['failed'])
        row['verdict'] = 'exact' if clean else 'differs'
        row['across'] = [o for o in row['word'] + row['zero'] if o < bound < o + 4]
    return sizes
