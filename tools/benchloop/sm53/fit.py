"""The device's results fitted family by family: clock, fixed and variable latency, issue cost, dual issue, reuse.

Each value carries its noise: a slope's standard error and residual, a stall's pattern of right (+) and wrong (-)
texels by delay. analyse() is the entry point; nak.py turns its fixed latencies into sm50.rs's instr_latency.

The one-warp families (ladder, chain, dual) are fitted twice: from the time per draw and from the single-draw time
(first_ns). Draws over one rect may overlap on the GPU, which shrinks the per-draw slope but not a lone draw's time;
`overlap` marks fits whose per-draw slope falls short of the single-draw one.
"""

import math
from collections import defaultdict
from collections.abc import Iterable

from tools.benchloop import protocol
from tools.benchloop.sm53 import measure, nak, stats
from tools.benchloop.sm53.latency import STALL_DELAYS
from tools.benchloop.sm53.timing_sim import SCHEDULERS

COLUMNS = (
    'id',
    'family',
    'op',
    'variant',
    'k',
    'delay',
    'warps',
    'status',
    'ns_per_draw',
    'residual_ns',
    'spread',
    'first_ns',
    'pixels_ok',
    'rel_err',
    'texel',
)
DUAL_MARGIN = 0.5
OVERLAP_TOLERANCE = 0.05


def _family(rows: list[dict], family: str) -> dict[str, list[dict]]:
    groups = defaultdict(list)
    for r in rows:
        if r.get('family') == family:
            groups[r.get('variant') or r['op']].append(r)
    return dict(groups)


def _timed(rows: Iterable[dict]) -> list[dict]:
    return [r for r in rows if r.get('timed')]


def clock(rows: list[dict]) -> dict:
    """Give ns per GPU cycle: the ladder's time per draw against k * delay (a lone warp issues one link per delay)."""
    ladder = _timed(r for r in rows if r.get('family') == 'ladder')
    if len({r['delay'] for r in ladder}) < 2:
        return {'clock_hz': protocol.CLOCK_HZ, 'ns_per_cycle': 1e9 / protocol.CLOCK_HZ, 'assumed': True}
    cycles = [r['k'] * r['delay'] for r in ladder]
    fit = stats.line(cycles, [r['ns_per_draw'] for r in ladder])
    first = stats.line(cycles, [r['first_ns'] for r in ladder])
    overlap = _overlap(fit, first)
    # draws of one warp overlap on the device's two SMs, so the lone draw's time is the clock then
    used = first if overlap else fit
    return {
        'clock_hz': 1e9 / used.slope,
        'ns_per_cycle': used.slope,
        'stderr_ns': used.stderr,
        'residual_ns': used.rms,
        'points': used.n,
        'ns_per_cycle_per_draw': fit.slope,
        'ns_per_cycle_first': first.slope,
        'overlap': overlap,
        'assumed': False,
    }


def _overlap(per_draw: stats.Line, first: stats.Line) -> bool:
    """Tell whether the per-draw slope falls short of the single-draw one beyond the tolerance and the noise."""
    excess = first.slope - per_draw.slope
    return excess > OVERLAP_TOLERANCE * per_draw.slope and excess > 3 * math.hypot(per_draw.stderr, first.stderr)


def _stall_reference(texels: dict[int, str | None], interpreted: str) -> tuple[str, str]:
    """Give the texel a long enough delay reads and where it came from.

    Below the latency every link reads its stale register, so every short delay gives one and the same wrong texel.
    A delay-15 texel that is neither the interpreter's nor that one means the hardware computes the op differently
    from the interpreter, and becomes the reference; one equal to it means 15 cycles are not enough.
    """
    longest, shortest = texels.get(max(STALL_DELAYS)), texels.get(min(STALL_DELAYS))
    if longest in (None, interpreted):
        return interpreted, 'interpreter'
    if longest == shortest:
        return interpreted, 'never right'
    return longest, 'delay-15'


def _stall_op(group: list[dict]) -> dict:
    ran = [r for r in group if r['status'] == 'ok']
    texels = {r['delay']: r.get('texel') for r in ran}
    reference, source = _stall_reference(texels, ran[0]['expected']) if ran else (None, 'no results')
    right = {d: t is not None and t == reference for d, t in texels.items()}
    latency = None
    for d in sorted(right, reverse=True):
        if not right[d]:
            break
        latency = d
    return {
        'latency': latency,
        'first_right': min((d for d in right if right[d]), default=None),
        'pattern': ''.join('?' if d not in right else '+' if right[d] else '-' for d in STALL_DELAYS),
        'reference': source,
        'pred': bool(group[0].get('pred')),
        'nak': group[0].get('nak'),
    }


def fixed(rows: list[dict]) -> dict[str, dict]:
    """Give each fixed-latency op's latency: the smallest delay from which every longer delay reads the right result."""
    return {op: _stall_op(group) for op, group in _family(rows, 'stall').items()}


def _slope(group: list[dict], y: str = 'ns_per_draw') -> stats.Line | None:
    timed = _timed(group)
    if len({r['k'] for r in timed}) < 2:
        return None
    return stats.line([r['k'] for r in timed], [r[y] for r in timed])


def _cycles(fit: stats.Line, scale: float) -> dict:
    return {'cycles': fit.slope * scale, 'stderr': fit.stderr * scale, 'residual_ns': fit.rms, 'points': fit.n}


def _one_warp(group: list[dict], ns_per_cycle: float) -> dict | None:
    fit, first = _slope(group), _slope(group, 'first_ns')
    if fit is None or first is None:
        return None
    return _cycles(fit, 1 / ns_per_cycle) | {
        'cycles_first': first.slope / ns_per_cycle,
        'overlap': _overlap(fit, first),
    }


def variable(rows: list[dict], ns_per_cycle: float) -> dict[str, dict]:
    """Give each variable-latency op's latency in cycles: the slope of its barrier chain's time against length."""
    out = {}
    for op, group in _family(rows, 'chain').items():
        fit = _one_warp(group, ns_per_cycle)
        if fit is not None:
            out[op] = fit | {'pixels_ok': all(r.get('pixels_ok') for r in group), 'nak': group[0].get('nak')}
    return out


def _issue(group: list[dict], clock_hz: float) -> dict | None:
    """Give scheduler cycles per warp-instruction: every warp of the draw shares SCHEDULERS issue slots a cycle."""
    fit = _slope(group)
    if fit is None:
        return None
    warps = group[0]['warps']
    return _cycles(fit, clock_hz * 1e-9 * SCHEDULERS / warps) | {'pixels_ok': all(r.get('pixels_ok') for r in group)}


def throughput(rows: list[dict], clock_hz: float) -> dict[str, dict]:
    out = {op: _issue(group, clock_hz) for op, group in _family(rows, 'throughput').items()}
    return {op: v for op, v in out.items() if v is not None}


def dual(rows: list[dict], ns_per_cycle: float) -> dict[str, dict]:
    """Give each pair's cycles per pair with X at delay 0 and at delay 1; dual when delay 0 saves half a cycle."""
    out = {}
    for op, group in _family(rows, 'dual').items():
        per_delay = {}
        for delay in (0, 1):
            fit = _one_warp([r for r in group if r['delay'] == delay], ns_per_cycle)
            if fit is not None:
                per_delay[delay] = fit
        if len(per_delay) == 2:
            c0, c1 = per_delay[0]['cycles'], per_delay[1]['cycles']
            out[op] = {'d0': per_delay[0], 'd1': per_delay[1], 'dual': c0 <= c1 - DUAL_MARGIN}
    return out


def reuse(rows: list[dict], clock_hz: float) -> dict:
    """Give FFMA's issue cycles per variant, the cost of each extra bank-0 read and what the reuse flags save."""
    variants = {v: _issue(group, clock_hz) for v, group in _family(rows, 'reuse').items()}
    out: dict = {v: x for v, x in variants.items() if x is not None}
    if {'spread', 'conflict'} <= out.keys():
        # 'conflict' reads three registers of one bank: two reads more than 'spread'
        out['bank_penalty'] = (out['conflict']['cycles'] - out['spread']['cycles']) / 2
    if {'conflict', 'reuse'} <= out.keys():
        out['reuse_saves'] = out['conflict']['cycles'] - out['reuse']['cycles']
    return out


def analyse(experiments: list[dict], results: dict[str, dict]) -> dict:
    """Give every family's fit from the experiments and the device's results by experiment id."""
    rows = [measure.row(e, results.get(e['id'])) for e in experiments]
    c = clock(rows)
    latencies = fixed(rows)
    return {
        'clock': c,
        'fixed': latencies,
        'variable': variable(rows, c['ns_per_cycle']),
        'throughput': throughput(rows, c['clock_hz']),
        'dual': dual(rows, c['ns_per_cycle']),
        'reuse': reuse(rows, c['clock_hz']),
        'nak': nak.table(latencies),
        'columns': list(COLUMNS),
        'rows': rows,
    }


def tsv(analysis: dict) -> str:
    """Give the per-experiment rows as tab-separated text, a header first."""

    def cell(v: object) -> str:
        if isinstance(v, float):
            return f'{v:.6g}'
        return '' if v is None else str(v)

    lines = ['\t'.join(analysis['columns'])]
    lines += ['\t'.join(cell(r.get(c)) for c in analysis['columns']) for r in analysis['rows']]
    return '\n'.join(lines) + '\n'
