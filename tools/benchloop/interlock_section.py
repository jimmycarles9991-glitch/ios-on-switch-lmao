"""Section shape with atomic ticket waits; emit with python -m tools.benchloop.interlock_section.

Only 64x64 overlaps the draws enough to judge lost updates. Larger targets measure time.
The load-spin control uses the whole section and only the 64-instruction body.
"""

import math
import statistics
import sys

from tools.benchloop import protocol, ticket
from tools.benchloop.sm53 import interlock, operands, sph

FORMS = ('whole', 'tail', 'none')
BODY_SIZES = (64, 256, 1024)
SIZES = ((64, 64), (256, 256), (1280, 720))


def experiment(form: str, body: int, width: int, height: int, timed: bool) -> dict:
    log_stride = (width - 1).bit_length()
    code = interlock.code(form, log_stride, body)
    # Four 720p draws keep the 1024-FFMA arm below benchloop's command-buffer work cap.
    counts = [1, 2, 4] if width == 1280 else [16, 32, 64]
    draws = counts if timed else [counts[-1]]
    repeats, warmup = (3, 1) if timed else (1, 0)
    return {
        'id': f'interlock_section.{width}x{height}.{body}.{form}.{"timed" if timed else "check"}',
        'kind': 'readback',
        'target': {'format': 'rgba32float', 'width': width, 'height': height},
        'program': {
            'sph': protocol.b64(sph.pixel(operands.attributes(code), reads_ticket=ticket.reads_ticket(code))),
            'code': protocol.b64(code),
            'data': protocol.b64(bytes(16)),
            'gprs': interlock.GPRS,
        },
        'draws': draws,
        'repeats': repeats,
        'warmup': warmup,
        'readback': 'none',
        'methods': [[interlock.INTERLOCK_CONTROL, 0 if form == 'none' else interlock.CONFLICT_DETECT_PIXEL]],
        'restore': [[interlock.INTERLOCK_CONTROL, 0]],
        'storage': {'bytes': (height << log_stride) * 4, 'va_at': 0, 'readback': 'summary'},
        'meta': {
            'family': 'interlock_section',
            'form': form,
            'body': body,
            'timed': timed,
            'correctness': (width, height) == (64, 64),
            'expect': sum(draws) * (repeats + warmup),
        },
    }


def generate() -> list[dict]:
    experiments = []
    for width, height in SIZES:
        for body in BODY_SIZES:
            forms = (*FORMS, 'load_spin') if body == 64 else FORMS
            for timed in (False, True):
                experiments.extend(experiment(form, body, width, height, timed) for form in forms)
    protocol.batch('interlock-section', experiments)
    return experiments


def _timing(e: dict, result: dict) -> dict:
    runs = result.get('runs', [])
    if [run['draws'] for run in runs] != e['draws']:
        raise ValueError('missing or unexpected draw counts')
    medians = []
    for run in runs:
        times = run['gpu_ns']
        if len(times) != e['repeats'] or any(
            not isinstance(t, (int, float)) or isinstance(t, bool) or not math.isfinite(t) or t <= 0 for t in times
        ):
            raise ValueError('missing or invalid GPU timestamps')
        medians.append(statistics.median(times))
    points = dict(zip(e['draws'], medians, strict=True))
    if len(points) == 1:
        slope = medians[0] / e['draws'][0]
    else:
        mx, my = statistics.fmean(points), statistics.fmean(medians)
        slope = sum((d - mx) * (t - my) for d, t in points.items()) / sum((d - mx) ** 2 for d in points)
    if slope <= 0:
        raise ValueError('nonpositive fitted GPU time per draw')
    return {'gpu_ms': {d: t / 1e6 for d, t in points.items()}, 'ns_per_draw': slope}


def _counts(e: dict, result: dict) -> dict:
    storage = result['storage']
    words = protocol.storage_words(result)
    if (
        storage['bytes'] != e['storage']['bytes']
        or not words
        or len(words) != len(storage['distinct'])
        or any(type(n) is not int or n <= 0 or not 0 <= v < 2**32 for v, n in words.items())
        or sum(words.values()) * 4 != storage['bytes']
    ):
        raise ValueError('missing or incomplete storage readback')
    target = protocol.target_of(e)
    pixels = target.width * target.height
    gaps = storage['bytes'] // 4 - pixels
    if words.get(0, 0) < gaps:
        raise ValueError('storage padding was overwritten')
    words[0] = words.get(0, 0) - gaps
    expect = sum(e['draws']) * (e['repeats'] + e['warmup'])
    return {
        'wrong_pixels': pixels - words.get(expect, 0),
        'lost_updates': sum(max(0, expect - value) * n for value, n in words.items()),
        'excess_updates': sum(max(0, value - expect) * n for value, n in words.items()),
    }


def analyse(experiments: list[dict], results: dict[str, dict]) -> dict:
    cells, gaps = {}, {}
    for e in experiments:
        if not e['id'].startswith('interlock_section.'):
            continue
        _, size, body, form, phase = e['id'].split('.')
        result = results.get(e['id']) or {}
        row = {'status': result.get('status', 'missing'), 'why': result.get('why', ''), 'passed': False}
        if row['status'] == 'ok':
            try:
                row.update(_timing(e, result))
                if size == '64x64':
                    row.update(_counts(e, result))
                    row['passed'] = row['excess_updates'] == 0 and (
                        row['lost_updates'] > 0 if form == 'none' else row['wrong_pixels'] == 0
                    )
                    row['verdict'] = 'lost-update control' if form == 'none' else 'zero lost updates'
                else:
                    row.update(passed=True, verdict='timing only')
            except (KeyError, ValueError, TypeError, OverflowError) as error:
                row.update(status='invalid', why=str(error))
        cells[e['id']] = row
        if phase == 'timed' and form in ('whole', 'tail'):
            gaps.setdefault(f'{size}.{body}', {})[form] = row
    differences = {}
    for group, arms in gaps.items():
        if set(arms) == {'whole', 'tail'} and all(row['passed'] for row in arms.values()):
            differences[group] = arms['whole']['ns_per_draw'] - arms['tail']['ns_per_draw']
    return {
        'cells': cells,
        'whole_minus_tail_ns': differences,
        'passed': bool(cells) and all(r['passed'] for r in cells.values()),
    }


if __name__ == '__main__':
    sys.stdout.buffer.write(protocol.encode_batch(protocol.batch('interlock-section', generate())))
