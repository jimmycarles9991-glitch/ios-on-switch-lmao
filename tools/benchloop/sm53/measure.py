"""One experiment's device result as one row: its time per draw and whether its pixels are what the host expects.

Time per draw is the least-squares slope of the median GPU time of each draw count against the count, which leaves
out each command buffer's fixed cost. Pixels pass when the rect holds one texel value, the expected one for an exact
experiment, and every texel outside it is the clear value.
"""

import math
import struct

from tools.benchloop import protocol
from tools.benchloop.sm53 import program, stats
from tools.benchloop.sm53.timing_sim import WARP

META = ('family', 'op', 'k', 'delay', 'variant', 'pred', 'nak')


def _timing(runs: list[dict]) -> dict:
    points = [(r['draws'], stats.median(r['gpu_ns'])) for r in runs if r.get('gpu_ns')]
    if len({d for d, _ in points}) < 2:
        return {'timed': False}
    per_draw = stats.line([d for d, _ in points], [t for _, t in points])
    first = min(points)[1]
    spread = stats.median([stats.relative_spread(r['gpu_ns']) for r in runs if r.get('gpu_ns')])
    return {
        'timed': True,
        'ns_per_draw': per_draw.slope,
        'residual_ns': per_draw.rms,
        'spread': spread,
        'first_ns': first,
    }


def _as_float(word: int) -> float:
    return struct.unpack('<f', struct.pack('<I', word))[0]


def _relative_error(observed: str, expected: list[int]) -> float | None:
    got = program.unpack(program.texel_words(observed))
    want = program.unpack(expected)
    errors = []
    for g, w in zip(got, want, strict=True):
        a, b = _as_float(g), _as_float(w)
        if not (math.isfinite(a) and math.isfinite(b)):
            return None if g != w else 0.0
        errors.append(abs(a - b) / max(abs(b), 1e-30))
    return max(errors)


def _pixels(e: dict, result: dict) -> dict:
    target = protocol.target_of(e)
    x0, y0, x1, y1 = protocol.rect_of(e, target)
    inside = (x1 - x0) * (y1 - y0)
    outside = target.width * target.height - inside
    clear = program.texel_hex(e['expect']['outside'])
    want = program.texel_hex(e['expect']['texel'])
    distinct = (result.get('texels') or {}).get('distinct') or []
    cleared = sum(d['count'] for d in distinct if d['value'] == clear)
    drawn = [d for d in distinct if d['value'] != clear]
    uniform = len(drawn) == 1 and drawn[0]['count'] == inside and cleared == outside
    observed = drawn[0]['value'] if uniform else None
    exact = e['expect']['exact']
    row = {'texel': observed, 'expected': want, 'uniform': uniform}
    row['pixels_ok'] = observed == want if exact else uniform
    if observed is not None and not exact:
        row['rel_err'] = _relative_error(observed, e['expect']['texel'])
    return row


def row(e: dict, result: dict | None) -> dict:
    """Give one experiment's row: its meta, status, timing and pixel check."""
    meta = e.get('meta', {})
    target = protocol.target_of(e)
    x0, y0, x1, y1 = protocol.rect_of(e, target)
    out = {'id': e['id'], **{k: meta[k] for k in META if k in meta}, 'warps': math.ceil((x1 - x0) * (y1 - y0) / WARP)}
    if 'pair' in meta:
        out['pair'] = tuple(meta['pair'])
    status = 'missing' if result is None else result.get('status', 'error')
    out['status'] = status
    if status != 'ok':
        return out | {'timed': False, 'pixels_ok': False, 'texel': None}
    return out | _timing(result.get('runs', [])) | _pixels(e, result)
