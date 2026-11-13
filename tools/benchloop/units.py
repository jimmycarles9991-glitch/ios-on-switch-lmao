"""Experiments on the units the first bench sessions could not reach, and what their results say.

compressed  the store sweep's values drawn into colour 0 in compressible memory, its texels copied in and out by the
            copy engine, against the same draws into a plain target (compare plain): the copy engine and the ROP on
            compressed kinds, as a histogram of differences per experiment.
cstall      the stall family as kernels that store their results (sm53/latency.py compute_stall): a delay's words are
            right or stale, so an op's latency is the smallest delay from which every longer one is right.
"""

from tools.benchloop import columns, protocol, rop
from tools.benchloop.sm53 import latency

COMPRESSED_FORMATS = ('bgra8unorm', 'rgba8unorm', 'rgb10a2unorm', 'rgba16float')
DECOMPRESS_SURFACE = 0x02E0
SIDE = 64
CHUNKS = 8


def compressed() -> list[dict]:
    out = []
    for fmt in COMPRESSED_FORMATS:
        values = rop.colours(fmt)
        for variant in ('noblend', 'one-one'):
            methods, restore, clear = rop.VARIANTS[variant]
            for part in range(CHUNKS):
                chunk = values[part * SIDE : (part + 1) * SIDE]
                if len(chunk) < SIDE:
                    break
                e = {
                    'id': f'comp-{fmt}-{variant}-{part}',
                    'kind': 'readback',
                    'target': {'format': fmt, 'width': SIDE, 'height': SIDE},
                    'program': columns.program(chunk),
                    'readback': 'full',
                    'clear': [clear] * 4,
                    'compressed': True,
                    'compare': 'plain',
                    'meta': {'family': 'compressed', 'variant': variant, 'colours': chunk},
                }
                if methods:
                    e |= {'methods': methods, 'restore': restore}
                out.append(e)
    first = next(e for e in out if e['id'] == 'comp-bgra8unorm-noblend-0')
    # DECOMPRESS_SURFACE colour target 0, array index 0, before the draw: accepted on a compressed target, or its error
    out.append(first | {'id': 'comp-bgra8unorm-decompress-0', 'methods': [[DECOMPRESS_SURFACE, 0]], 'restore': []})
    return out


def analyse_compressed(experiments: list[dict], results: dict[str, dict]) -> list[dict]:
    rows = []
    for e in experiments:
        r = results.get(e['id']) or {}
        compare = r.get('compare') or {}
        histogram = compare.get('histogram') or {}
        rows.append(
            {
                'id': e['id'],
                'status': r.get('status'),
                'why': r.get('why', ''),
                'form': r.get('form'),
                'compare': compare.get('status'),
                'compare_why': compare.get('why', ''),
                'identical': bool(histogram) and set(histogram) == {'0'},
                'histogram': histogram,
            }
        )
    return rows


def cstall() -> list[dict]:
    return latency.compute_stall()


def output_words(result: dict) -> list[int]:
    data = protocol.unb64((result.get('output') or {}).get('data') or '')
    return [int.from_bytes(data[i : i + 4], 'little') for i in range(0, len(data) - 3, 4)]


def analyse_cstall(experiments: list[dict], results: dict[str, dict]) -> dict:
    """Give each experiment's verdict and per op its latency: the least delay from which every longer one is right."""
    rows: list[dict] = []
    right_at: dict[str, dict[int, bool]] = {}
    for e in experiments:
        r = results.get(e['id']) or {}
        right = r.get('status') == 'ok' and output_words(r) == e['expect']['words']
        rows.append({'id': e['id'], 'status': r.get('status'), 'right': right, 'why': r.get('why', '')})
        right_at.setdefault(e['meta']['op'], {})[e['meta']['delay']] = right
    latencies = {}
    for op, by_delay in right_at.items():
        delays = sorted(by_delay)
        wrong = [d for d in delays if not by_delay[d]]
        latencies[op] = (max(wrong) + 1 if wrong else delays[0]) if delays else None
    return {'rows': rows, 'latency': latencies}
