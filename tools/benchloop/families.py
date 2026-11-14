"""The analyses of a `run` of batches/*.json, chosen by the families their experiments name, and one line per answer."""

from tools.benchloop import store2fma
from tools.benchloop.sm53 import affinity, fit, hsetp2, icache, int3, interlock_spin, measure, overlap

# The clock the ladder measured (loop1, 11 Nov), for a run without a ladder of its own
MEASURED_CLOCK_HZ = 769.1e6
TIMING = {'ladder', 'stall', 'throughput'}


def clock_hz(planned: list[dict], results: dict[str, dict]) -> float:
    ladder = [x for x in planned if x['meta'].get('family') == 'ladder']
    if not ladder:
        return MEASURED_CLOCK_HZ
    c = fit.clock([measure.row(x, results.get(x['id'])) for x in ladder])
    return MEASURED_CLOCK_HZ if c['assumed'] else c['clock_hz']


# family: (report key, its analysis of the run's experiments and results at the clock)
STEPS = {
    'stall-wide': ('stall_wide', lambda planned, results, _: int3.analyse_wide(planned, results)),
    'xmad-mul': ('xmad_mul', lambda planned, results, _: int3.analyse_mul(planned, results)),
    'icache': ('icache', icache.analyse),
    'overlap': ('overlap', overlap.analyse),
    'affinity': ('affinity', lambda planned, results, _: affinity.analyse(planned, results)),
    'store2fma': ('store2fma', lambda planned, results, _: store2fma.analyse(planned, results)),
    'hsetp2': ('hsetp2', lambda planned, results, _: hsetp2.analyse(planned, results)),
    'interlock': ('interlock', lambda planned, results, _: interlock_spin.analyse(planned, results)),
}


def _families(planned: list[dict]) -> set:
    return {x['meta'].get('family') for x in planned}


def asked(planned: list[dict]) -> bool:
    """Say whether a run holds any experiment of a family answered here."""
    return bool(_families([x for x in planned if x.get('meta')]) & (TIMING | set(STEPS)))


def analyse(planned: list[dict], results: dict[str, dict]) -> dict:
    planned = [x for x in planned if x.get('meta')]
    families = _families(planned)
    hz = clock_hz(planned, results)
    out: dict = {'clock_hz': hz}
    if families & TIMING:
        found = fit.analyse([x for x in planned if x['meta'].get('family') in TIMING], results)
        out['latency'] = {k: found[k] for k in ('clock', 'fixed', 'throughput', 'nak')}
    for family, (key, run) in STEPS.items():
        if family in families:
            out[key] = run(planned, results, hz)
    out['summary'] = summary(out)
    return out


def _f(v: float | None, digits: int = 2) -> str:
    return '-' if v is None else f'{v:.{digits}f}'


def _latency_lines(found: dict) -> list[str]:
    lines = [f'clock {found["clock"].get("clock_hz", 0) / 1e6:.1f} MHz']
    for op, s in found['fixed'].items():
        lines.append(f'stall {op} {s["pattern"]} latency {s["latency"]} (NAK {s["nak"]})')
    lines += [f'throughput {op} {_f(t["cycles"])} cycles/warp-instr' for op, t in found['throughput'].items()]
    return lines


def _wide(found: dict) -> list[str]:
    return [f'stall-wide {op} {w["pattern"]} right from {w["right_from"]}' for op, w in found.items()]


def _mul(found: dict) -> list[str]:
    return [f'xmad-mul {n} {m["checked"]} pairs, {m["wrong"][0]}/{m["wrong"][1]} words wrong' for n, m in found.items()]


def _icache(found: list) -> list[str]:
    return [f'icache {r["code_kib"]:.1f} KiB {_f(r.get("cycles_per_warp_instr"), 3)} cycles/warp-instr' for r in found]


def _overlap(found: dict) -> list[str]:
    return [
        f'overlap {name} {_f(m["measured"])} (overlap {_f(m["overlap"])}, add {_f(m["add"])}) {m["closer"]}'
        for name, m in found.items()
        if name != 'cycles'
    ]


def _affinity(found: dict) -> list[str]:
    return [
        f'{eid} max {a["max"]} histogram {a["histogram"]} buffer {a["ticket_buffer_kib"]} KiB'
        if a['status'] == 'ok'
        else f'{eid} {a["status"]} {a["why"]}'
        for eid, a in found.items()
    ]


def _store(found: dict) -> list[str]:
    return [
        f'store2fma {fmt} {s["checked"]} values, {s["off_metal"]} off Metal, {len(s["missing"])} missing'
        for fmt, s in found.items()
    ]


def _interlock(found: dict) -> list[str]:
    def line(eid: str, r: dict) -> str:
        head = f'{eid} {r["status"]} lost {r.get("lost", "-")} ns/draw {_f(r.get("ns_per_draw"), 0)}'
        return head + (f' competitor {r["competitor_buffers"]} buffers' if 'competitor_buffers' in r else '')

    return [line(eid, r) for eid, r in found.items()]


def _hsetp2(found: dict) -> list[str]:
    lines = []
    for eid, v in found.items():
        if v['status'] != 'ok':
            lines.append(f'{eid} {v["status"]} {v["why"]}')
            continue
        off = {name: r for name, r in v['forms'].items() if r['p_off'] or r['q_off']}
        total = sum(r['p_off'] + r['q_off'] for r in off.values())
        lines.append(f'{eid} {len(v["forms"])} forms x {v["columns"]} pairs, {total} predicates off the interpreter')
        lines += [f'  {name}: P off {r["p_off"]}, Q off {r["q_off"]}, first {r["first"]}' for name, r in off.items()]
    return lines


LINES = {
    'stall_wide': _wide,
    'xmad_mul': _mul,
    'icache': _icache,
    'overlap': _overlap,
    'affinity': _affinity,
    'store2fma': _store,
    'interlock': _interlock,
    'hsetp2': _hsetp2,
}


def summary(out: dict) -> list[str]:
    lines = _latency_lines(out['latency']) if 'latency' in out else []
    return lines + [line for key, lines_of in LINES.items() if key in out for line in lines_of(out[key])]
