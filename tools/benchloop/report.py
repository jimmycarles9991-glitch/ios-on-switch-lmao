"""A run's tables.

Every experiment's GPU time per draw and texel check (results.tsv), then the family's own analysis
(the latency fit, the blend and rounding checks) in report.json.
"""

import json
import statistics
import struct
from pathlib import Path

from tools.benchloop import blend, cbufsize, floatadd, qmdchain, rop, sweeps, twod, units


def per_draw(result: dict) -> tuple[float | None, float | None, float | None]:
    """Least squares of the median GPU time against draws: ns a draw, ns fixed, and the largest residual."""
    points = [(run['draws'], statistics.median(run['gpu_ns'])) for run in result.get('runs', []) if run.get('gpu_ns')]
    if len(points) < 2 or len({d for d, _ in points}) < 2:
        return (points[0][1] / max(1, points[0][0]), None, None) if points else (None, None, None)
    mx = statistics.fmean(d for d, _ in points)
    my = statistics.fmean(t for _, t in points)
    slope = sum((d - mx) * (t - my) for d, t in points) / sum((d - mx) ** 2 for d, _ in points)
    fixed = my - slope * mx
    return slope, fixed, max(abs(t - fixed - slope * d) for d, t in points)


def texels_match(experiment: dict, result: dict) -> str:
    expect = experiment.get('expect')
    if not expect or 'texel' not in expect:
        return '-'
    distinct = (result.get('texels') or {}).get('distinct')
    if not distinct:
        return 'no texels'
    want = {struct.pack('<4I', *expect['texel']).hex(), struct.pack('<4I', *expect.get('outside', [0] * 4)).hex()}
    wrong = [d for d in distinct if d['value'] not in want]
    return 'right' if not wrong else f'wrong:{wrong[0]["value"]}@{wrong[0]["first"]}'


def rows(planned: list[dict], results: dict[str, dict]) -> list[list[str]]:
    out = [['id', 'kind', 'status', 'ns_per_draw', 'ns_fixed', 'residual_ns', 'texels', 'why']]
    for e in planned:
        r = results.get(e['id'], {'status': 'missing'})
        slope, fixed, residual = per_draw(r)
        cells = [slope, fixed, residual]
        out.append(
            [
                e['id'],
                e['kind'],
                r['status'],
                *('-' if c is None else f'{c:.1f}' for c in cells),
                texels_match(e, r),
                r.get('why', ''),
            ]
        )
    return out


def analysis(command: str, planned: list[dict], results: dict[str, dict], inputs: list[Path]) -> dict:
    if command == 'latency':
        from tools.benchloop.sm53 import fit  # noqa: PLC0415  the fit pulls in the interpreter

        return fit.analyse(planned, results)
    if command == 'blend':
        tables = {t.name: t for t in (blend.Table.load(p) for p in inputs)}
        return {
            'blend': [
                blend.check(tables[e['meta']['table']], e, results.get(e['id'], {'status': 'missing'})) for e in planned
            ]
        }
    if command in ('rop', 'store16'):
        return {command: [rop.check(e, results.get(e['id'], {'status': 'missing'})) for e in planned]}
    if command == 'floatadd':
        return {'floatadd': [floatadd.check(e, results.get(e['id'], {'status': 'missing'})) for e in planned]}
    sweep = {
        'precision': sweeps.analyse_precision,
        'iterated': sweeps.analyse_iterated,
        'twod': twod.analyse,
        'compressed': units.analyse_compressed,
        'cstall': units.analyse_cstall,
        'qmdchain': qmdchain.analyse,
        'cbufsize': cbufsize.analyse,
    }
    return {command: sweep[command](planned, results)} if command in sweep else {}


def write(command: str, planned: list[dict], results: dict[str, dict], out: Path, inputs: list[Path]) -> str:
    table = rows(planned, results)
    (out / 'results.tsv').write_text('\n'.join('\t'.join(row) for row in table) + '\n')
    found = analysis(command, planned, results, inputs)
    (out / 'report.json').write_text(json.dumps(found, indent=1, default=str) + '\n')
    if command == 'latency' and 'rust' in found.get('nak', {}):
        (out / 'sm50_latency.rs').write_text(found['nak']['rust'])
    statuses: dict[str, int] = {}
    for row in table[1:]:
        statuses[row[2]] = statuses.get(row[2], 0) + 1
    wrong = sum(row[6].startswith('wrong') for row in table[1:])
    return f'{command}: {len(planned)} experiments, {statuses}, {wrong} with wrong texels'
