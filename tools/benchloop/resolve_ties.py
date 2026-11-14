"""Batch resolvetie2: how the 2D engine rounds an 8-bit MSAA resolve, and whether its complement gives Metal's rule.

Metal resolves n samples to (sum + n/2) / n. The engine's box of the sample grid (the driver's resolve: corner origin,
du = 2 a sampled axis, origin at half a step, bilinear) lands one code under it on ties. Each cell reads a real
multisampled image whose grid the bench copies from the cell's source, so every pixel's samples are known:

- prod: the driver's reading by a scale, on sources that put every sum (4x) or every sample pair (2x) in every channel,
  and on 4x tuples whose pairs both sum odd, where one rounding and two 2:1 steps disagree;
- drv: the driver's own resolve against the pack's resolve pass (compare resolve3d);
- sandwich: the grid complemented in place (ROP3 DSTINVERT), then read under ROP3 NOTSRCCOPY: 255 - down(255n - s)
  is up(s) for every sum, so one ties-down rounding becomes Metal's rule;
- controls: a 1:1 NOTSRCCOPY copy, the driver's reading under NOTSRCCOPY alone, a point read of the complemented grid;
- the sweep: every sample mode, filter and 1/16-texel origin at 2x and 4x, for any setting that is Metal's rule.

analyse() gives each cell's mismatches per rounding model, read through the cell's complements; verdict() the
conclusions. Run either `bazel run //tools/benchloop -- resolveties` or `... -- run "$PWD/tools/benchloop/batches/
resolvetie2.json"` (this file's write()), with an app at benchloop/6.
"""

import json
import math
import random
import sys

from tools.benchloop import protocol, twod
from tools.benchloop.resolve_verdict import verdict

FORMATS = ('bgra8unorm', 'rgba8unorm')
NOT_SRC = 0x33
GRIDS = protocol.SAMPLE_GRIDS
ONE = protocol.ONE
SIXTEENTH = 1 << 28
SWEEP = {True: range(-8, 9), False: range(17)}
MODELS = {
    2: ('up', 'down'),
    4: ('up', 'down', 'trunc', 'pair_x_down', 'pair_y_down', 'pair_x_up', 'pair_y_up'),
}


def _grid(tuples: list[list[tuple[int, ...]]], w: int, h: int, n: int) -> bytes:
    """Give the sample grid of a w x h target: tuples[c][p] are pixel p's samples in channel c, in (i, j) row order."""
    sx, sy = GRIDS[n]
    gw = w << sx
    out = bytearray(gw * (h << sy) * 4)
    for p in range(w * h):
        x, y = p % w, p // w
        for k in range(n):
            i, j = k & 1, k >> 1
            at = (((y << sy) + j) * gw + (x << sx) + i) * 4
            for c in range(4):
                out[at + c] = tuples[c][p][k]
    return bytes(out)


def _split(total: int, parts: int, rng: random.Random) -> tuple[int, ...]:
    """Give `parts` codes summing to `total`, drawn so the largest moves between positions."""
    left, out = total, []
    for k in range(parts - 1):
        rest = parts - 1 - k
        lo, hi = max(0, left - 255 * rest), min(255, left)
        out.append(rng.randint(lo, hi))
        left -= out[-1]
    out.append(left)
    rng.shuffle(out)
    return tuple(out)


def exhaustive2() -> list[list[tuple[int, ...]]]:
    """Give every (a, b) once across the four channels: channel c holds a in [64c, 64c + 64) against every b."""
    return [[(64 * c + (p >> 8), p & 255) for p in range(128 * 128)] for c in range(4)]


def exhaustive4(seed: int = 7) -> list[list[tuple[int, ...]]]:
    """Give every sum 0..1020 at least four times a channel, split differently each time."""
    rng = random.Random(seed)
    return [[_split((p + 257 * c) % 1021, 4, rng) for p in range(64 * 64)] for c in range(4)]


def odd_pairs(axis: str, seed: int = 11) -> list[list[tuple[int, ...]]]:
    """Give 4x tuples whose two pair sums along `axis` ('x': (0,0)+(1,0), (0,1)+(1,1); 'y' the columns) are odd."""
    rng = random.Random(seed)
    out = []
    for c in range(4):
        tuples = []
        for p in range(64 * 64):
            first, second = 2 * ((p * 37 + c * 61) % 255) + 1, 2 * ((p * 101 + c * 29) % 255) + 1
            (a, b), (d, e) = _split(first, 2, rng), _split(second, 2, rng)
            tuples.append((a, b, d, e) if axis == 'x' else (a, d, b, e))
        out.append(tuples)
    return out


def random_tuples(n: int, w: int, h: int, seed: int) -> list[list[tuple[int, ...]]]:
    rng = random.Random(seed)
    return [[tuple(rng.randrange(256) for _ in range(n)) for _ in range(w * h)] for _ in range(4)]


def _source(fmt: str, tuples: list[list[tuple[int, ...]]], w: int, h: int, n: int) -> dict:
    sx, sy = GRIDS[n]
    data = protocol.b64(_grid(tuples, w, h, n))
    return {'format': fmt, 'width': w << sx, 'height': h << sy, 'data': data}


def production(n: int) -> dict:
    sx, sy = GRIDS[n]
    du, dv = ONE << sx, ONE << sy
    return {'du': du, 'dv': dv, 'sx': du // 2, 'sy': dv // 2, 'bilinear': True, 'corner': True}


def _cell(eid: str, fmt: str, tuples: list, reading: str, op: dict) -> dict:
    """Give a resolve of the square target `tuples` covers, `op` its twod fields beside op and samples."""
    n, side = len(tuples[0][0]), math.isqrt(len(tuples[0]))
    e = twod._experiment(
        eid, fmt, (side, side), {'op': 'resolve', 'samples': n} | op, source=_source(fmt, tuples, side, side, n)
    )
    e.pop('fill')
    return e | {'meta': {'family': 'resolvetie2', 'reading': reading, 'samples': n}}


def cells(fmt: str) -> list[dict]:
    sources = {2: exhaustive2(), 4: exhaustive4()}
    pairs = {'xpairs': odd_pairs('x'), 'ypairs': odd_pairs('y')}
    out = []
    for n in (2, 4):
        by = {'byScale': True} | production(n)
        point = by | {'bilinear': False, 'sx': ONE // 2, 'sy': ONE // 2}
        variants = [('', sources[n])] + ([(f'-{k}', v) for k, v in pairs.items()] if n == 4 else [])
        for tag, tuples in variants:
            out.append(_cell(f'rt-{fmt}-{n}x-prod{tag}', fmt, tuples, 'plain', by))
            sandwich = by | {'rop': NOT_SRC, 'invertGrid': True}
            out.append(
                _cell(f'rt-{fmt}-{n}x-sandwich{tag}', fmt, tuples, 'sandwich', sandwich) | {'compare': 'resolve3d'}
            )
        out.append(_cell(f'rt-{fmt}-{n}x-drv', fmt, sources[n], 'plain', {}) | {'compare': 'resolve3d'})
        out.append(_cell(f'rt-{fmt}-{n}x-notonly', fmt, sources[n], 'notonly', by | {'rop': NOT_SRC}))
        out.append(_cell(f'rt-{fmt}-{n}x-invpoint', fmt, sources[n], 'invpoint', point | {'invertGrid': True}))
    copy = twod._experiment(f'rt-{fmt}-not-1to1', fmt, (64, 64), {'op': 'scale', 'rop': NOT_SRC})
    random_texels = twod.random_source(fmt, 64, seed=13)
    out.append(copy | {'source': random_texels, 'meta': {'family': 'resolvetie2', 'reading': 'not1to1', 'samples': 1}})
    return out


def sweep(n: int) -> list[dict]:
    """Give the grid read at every sample mode, filter and origin offset in 1/16 texel along the sampled axes."""
    _, sy = GRIDS[n]
    size = (32, 32)
    tuples = random_tuples(n, *size, seed=17 + n)
    out = []
    for corner, offsets in SWEEP.items():
        for bilinear in (True, False):
            for k in offsets:
                origin = (ONE if corner else 0) + k * SIXTEENTH
                op = production(n) | {'byScale': True, 'bilinear': bilinear, 'corner': corner, 'sx': origin}
                op['sy'] = origin if sy else (ONE // 2 if corner else 0)
                eid = f'rt-sweep{n}-{"corner" if corner else "centre"}-{"bil" if bilinear else "point"}-'
                eid += f'{"m" if k < 0 else "p"}{abs(k):02d}'
                out.append(_cell(eid, 'bgra8unorm', tuples, 'plain', op))
    return out


def generate() -> list[dict]:
    return [e for fmt in FORMATS for e in cells(fmt)] + sweep(4) + sweep(2)


def batch() -> dict:
    return protocol.batch('resolvetie2', generate())


def write(path: str) -> None:
    """Write the batch with its host-side meta, as `benchloop run` reads one, in the batch files' layout."""
    with open(path, 'w') as f:
        json.dump(batch(), f, sort_keys=True, indent=2)
        f.write('\n')


def model(name: str, s: tuple[int, ...]) -> int:
    if len(s) == 2:
        return (s[0] + s[1] + (name == 'up')) >> 1
    total = sum(s)
    if name in ('up', 'down', 'trunc'):
        return (total + {'up': 2, 'down': 1, 'trunc': 0}[name]) >> 2
    bias = int(name.endswith('_up'))
    first, second = (s[0] + s[1], s[2] + s[3]) if name.startswith('pair_x') else (s[0] + s[2], s[1] + s[3])
    return (((first + bias) >> 1) + ((second + bias) >> 1) + bias) >> 1


def _tuples(e: dict) -> list[list[tuple[int, ...]]]:
    """Give the cell's samples back from its source: [channel][pixel] tuples in (i, j) row order."""
    n, w, h = e['twod']['samples'], e['target']['width'], e['target']['height']
    sx, sy = GRIDS[n]
    grid, gw = protocol.unb64(e['source']['data']), e['source']['width']
    out = [[] for _ in range(4)]
    for p in range(w * h):
        x, y = p % w, p // w
        at = [(((y << sy) + (k >> 1)) * gw + (x << sx) + (k & 1)) * 4 for k in range(n)]
        for c in range(4):
            out[c].append(tuple(grid[a + c] for a in at))
    return out


def picked(tuples: list[list[tuple[int, ...]]], data: bytes) -> int | None:
    """Give the sample index a point read returned at every pixel and channel; None if no one sample did.

    A point cell's positive control: a point read copies a sample, so None means the grid or its reading is wrong.
    """
    for k in range(len(tuples[0][0])):
        if all(data[p * 4 + c] == tuples[c][p][k] for c in range(4) for p in range(len(tuples[0]))):
            return k
    return None


def judge(e: dict, result: dict) -> dict:
    """Give a cell's row: mismatches per model through its reading's complements, against Metal, and its controls."""
    meta, data = e['meta'], protocol.unb64(((result.get('texels') or {}).get('data')) or '')
    row = {'id': e['id'], 'status': result.get('status'), 'reading': meta['reading'], 'samples': meta['samples']}
    compare = result.get('compare') or {}
    if 'compare' in e:
        row['compare'] = {'status': compare.get('status'), 'histogram': compare.get('histogram')}
    form = result.get('form') or {}
    t = e['twod']
    row['form_ok'] = form.get('rop', 0) == t.get('rop', 0) and bool(form.get('invert_grid')) == bool(
        t.get('invertGrid')
    )
    if not data:
        return row
    if meta['reading'] == 'not1to1':
        src = protocol.unb64(e['source']['data'])
        row['exact'] = all(data[i] == 255 - src[i] for i in range(len(data)))
        return row
    tuples, n = _tuples(e), meta['samples']
    flip_in, flip_out = meta['reading'] == 'sandwich', meta['reading'] in ('sandwich', 'notonly')
    if meta['reading'] == 'invpoint':
        row['exact'] = all(data[p * 4 + c] == 255 - tuples[c][p][0] for c in range(4) for p in range(len(tuples[0])))
        return row
    if not t.get('bilinear', True) and t.get('byScale'):
        row['point_sample'] = picked(tuples, data)
    misses, residues, metal = dict.fromkeys(MODELS[n], 0), {}, 0
    for c in range(4):
        for p, s in enumerate(tuples[c]):
            got = data[p * 4 + c]
            seen = 255 - got if flip_out else got
            read = tuple(255 - v for v in s) if flip_in else s
            for name in MODELS[n]:
                misses[name] += model(name, read) != seen
            metal += got != model('up', s)
            r = residues.setdefault(sum(s) % n, {'channels': 0, 'up': 0, 'down': 0})
            r['channels'] += 1
            r['up'] += got == model('up', s)
            r['down'] += got == model('down', s)
    return row | {'models': misses, 'metal_mismatches': metal, 'residues': residues}


def _texels(results: dict[str, dict], eid: str) -> bytes:
    return protocol.unb64((((results.get(eid) or {}).get('texels')) or {}).get('data') or '')


def analyse(experiments: list[dict], results: dict[str, dict]) -> dict:
    """Give each cell's row, a NOTSRCCOPY-only cell held to the complement of its plain twin, and the verdict."""
    rows = []
    for e in experiments:
        if (e.get('meta') or {}).get('family') != 'resolvetie2':
            continue
        row = judge(e, results.get(e['id']) or {})
        if row['reading'] == 'notonly':
            own, twin = _texels(results, e['id']), _texels(results, e['id'].replace('-notonly', '-prod'))
            row['complements_prod'] = (
                bool(own) and len(own) == len(twin) and all(a == 255 - b for a, b in zip(own, twin, strict=True))
            )
        rows.append(row)
    return {'rows': rows, 'verdict': verdict(rows)}


if __name__ == '__main__':
    write(sys.argv[1])
