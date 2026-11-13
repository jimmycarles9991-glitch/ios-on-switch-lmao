"""Measured fixed latencies against NAK's sm50.rs instr_latency, and the same table rebuilt from the measurements.

instr_latency gives each Op a (gpr_latency, pred_latency) pair: the half ops (13, 14), everything else (6, 13). A
measurement fills the half an op writes (pred for FSETP/ISETP, gpr for the rest); the other half keeps NAK's value
and is marked unmeasured. Keys that share an Op (IADD and IADD32I are both IAdd2) take the larger measurement. An op
never right within the longest delay keeps NAK's value in the table and is reported unsafe.
"""

from collections import defaultdict

NAK_HALF_OPS = {'HAdd2', 'HFma2', 'HMul2', 'HSet2', 'HSetP2', 'HMnMx2', 'DAdd', 'DFma', 'DMnMx', 'DMul', 'DSetP'}
NAK_HALF = (13, 14)
NAK_DEFAULT = (6, 13)
NEVER_RIGHT = '>15'


def current(op: str) -> tuple[int, int]:
    return NAK_HALF if op in NAK_HALF_OPS else NAK_DEFAULT


def _measured(fit: dict) -> int | str | None:
    """Give a stall fit's latency, NEVER_RIGHT when no delay read the right result, None when nothing ran."""
    if fit.get('nak') is None:
        return None
    if fit.get('latency') is None:
        return NEVER_RIGHT if fit.get('reference') == 'never right' else None
    return fit['latency']


def _entries(latencies: dict[str, dict]) -> dict[str, dict]:
    entries: dict[str, dict] = defaultdict(lambda: {'gpr': None, 'pred': None, 'keys': {}})
    for key, fit in latencies.items():
        measured = _measured(fit)
        if measured is None:
            continue
        entry = entries[fit['nak']]
        entry['keys'][key] = measured
        half = 'pred' if fit['pred'] else 'gpr'
        if measured != NEVER_RIGHT:
            entry[half] = max(entry[half] or 0, measured)
    return dict(entries)


def _disagreements(latencies: dict[str, dict]) -> list[dict]:
    out = []
    for key, fit in sorted(latencies.items()):
        measured = _measured(fit)
        if measured is None:
            continue
        half = 'pred' if fit['pred'] else 'gpr'
        nak = current(fit['nak'])[half == 'pred']
        if measured != nak:
            kind = 'unsafe' if measured == NEVER_RIGHT or measured > nak else 'slack'
            out.append({'op': key, 'nak_op': fit['nak'], 'half': half, 'nak': nak, 'measured': measured, 'kind': kind})
    return out


def rust(entries: dict[str, dict]) -> str:
    """Give the measured table as sm50.rs's instr_latency match, ops grouped by their (gpr, pred) pair.

    The half ops nothing measured keep NAK's arm, so the match can replace NAK's as it stands.
    """
    groups = defaultdict(list)
    for op, entry in sorted(entries.items()):
        gpr, pred = current(op)
        groups[(entry['gpr'] or gpr, entry['pred'] or pred)].append(op)
    groups[NAK_HALF] += sorted(NAK_HALF_OPS - entries.keys())
    lines = [
        "// Measured on GM20B; a half an op never writes keeps NAK's value.",
        'let (gpr_latency, pred_latency) = match op {',
    ]
    for (gpr, pred), ops in sorted(groups.items()):
        arms = [f'Op::{op}(_)' for op in ops]
        lines.append('    ' + arms[0])
        lines += [f'    | {a}' for a in arms[1:]]
        lines[-1] += f' => ({gpr}, {pred}),'
    lines += [f'    _ => {NAK_DEFAULT},', '};']
    return '\n'.join(lines) + '\n'


def table(latencies: dict[str, dict]) -> dict:
    """Give the fitted instr_latency (JSON and Rust), the ops NAK disagrees with, and keys of one Op that differ."""
    entries = _entries(latencies)
    json_form = {
        op: {
            'gpr': e['gpr'] if e['gpr'] is not None else current(op)[0],
            'pred': e['pred'] if e['pred'] is not None else current(op)[1],
            'measured': {'gpr': e['gpr'] is not None, 'pred': e['pred'] is not None},
            'keys': e['keys'],
        }
        for op, e in sorted(entries.items())
    }
    split = {op: e['keys'] for op, e in entries.items() if len(set(e['keys'].values())) > 1}
    return {
        'json': json_form,
        'rust': rust(entries),
        'disagreements': _disagreements(latencies),
        'split_ops': split,
    }
