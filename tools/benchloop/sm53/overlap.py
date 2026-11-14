"""Whether instruction classes issue in parallel: FFMA interleaved with F2F, SHL, MUFU.RCP or ISETP, beside each pure.

Every stream is a looped body of BODY instructions over the whole target, run k = BODY x LOOPS times, with the same
registers and register count (so the same occupancy). A mix whose cycles per warp-instruction sit near the busiest
class's share overlaps; one near the sum of its classes' shares adds.
"""

from tools.benchloop.sm53 import encode as e, fit, latency, measure, program
from tools.benchloop.sm53.encode import PT, Imm, f32
from tools.benchloop.sm53.ops import K1, K2, SRC

BODY = 256
LOOPS = (2, 4, 8)
GPRS = 72
FFMA_REGS = [8 + 4 * j for j in range(16)]
OTHER_REGS = [9 + 4 * j for j in range(16)]
PREDICATES = 6


def _ffma(i: int) -> e.Instr:
    r = FFMA_REGS[i % 16]
    return e.ffma(r, r, K1, K2)


def _shl(i: int) -> e.Instr:
    r = OTHER_REGS[i % 16]
    return e.shl(r, r, Imm(1))


def _f2f(i: int) -> e.Instr:
    return e.f2f(OTHER_REGS[i % 8], SRC, 'round').sched(wr_bar=0)


def _rcp(i: int) -> e.Instr:
    return e.mufu(OTHER_REGS[i % 8], SRC, 'rcp').sched(wr_bar=0)


def _isetp(i: int) -> e.Instr:
    return e.isetp(i % PREDICATES, K1, K2, 'lt s32 xor', PT)


CLASSES = {'FFMA': _ffma, 'F2F': _f2f, 'SHL': _shl, 'MUFU.RCP': _rcp, 'ISETP': _isetp}
# stream: the classes of one repeating group, in issue order
STREAMS = {
    'FFMA': ('FFMA',),
    'F2F': ('F2F',),
    'SHL': ('SHL',),
    'MUFU.RCP': ('MUFU.RCP',),
    'ISETP': ('ISETP',),
    'FFMA3-F2F1': ('FFMA', 'FFMA', 'FFMA', 'F2F'),
    'FFMA1-SHL1': ('FFMA', 'SHL'),
    'FFMA3-MUFU1': ('FFMA', 'FFMA', 'FFMA', 'MUFU.RCP'),
    'FFMA1-ISETP1': ('FFMA', 'ISETP'),
}
MIXES = {name: group for name, group in STREAMS.items() if len(group) > 1}


def body(group: tuple[str, ...]) -> list[e.Instr]:
    """Give BODY instructions cycling through `group`, each class over its own registers in turn."""
    seen = dict.fromkeys(group, 0)
    out = []
    for i in range(BODY):
        cls = group[i % len(group)]
        out.append(CLASSES[cls](seen[cls]))
        seen[cls] += 1
    return out


def experiment(name: str, loops: int) -> dict:
    init = {K1: f32(1.0), K2: f32(0.5), SRC: f32(2.5)} | dict.fromkeys(FFMA_REGS, 0) | dict.fromkeys(OTHER_REGS, 1)
    prog = program.fragment(body(STREAMS[name]), init, (FFMA_REGS[0], OTHER_REGS[0]), loop=loops)
    meta = {'family': 'overlap', 'op': name, 'variant': name, 'k': BODY * loops, 'group': list(STREAMS[name])}
    eid = f'overlap.{name.lower()}.k{BODY * loops:04d}'
    x = latency.experiment(eid, prog, latency.WHOLE, meta, exact='MUFU.RCP' not in STREAMS[name])
    x['program']['gprs'] = GPRS
    return x


def generate() -> list[dict]:
    return [latency.warm_up('overlap.warm'), *(experiment(name, n) for name in STREAMS for n in LOOPS)]


def predictions(rates: dict[str, float], group: tuple[str, ...]) -> dict[str, float]:
    """Give a mix's cycles per warp-instruction if its classes overlap (the busiest decides) and if they add."""
    share = {cls: group.count(cls) * rates[cls] for cls in set(group)}
    return {'overlap': max(share.values()) / len(group), 'add': sum(share.values()) / len(group)}


def analyse(experiments: list[dict], results: dict[str, dict], clock_hz: float) -> dict:
    """Give each stream's measured cycles per warp-instruction and, for each mix, both models from the pure ones."""
    rows = [measure.row(x, results.get(x['id'])) for x in experiments if x['meta'].get('family') == 'overlap']
    issue = fit.throughput([r | {'family': 'throughput'} for r in rows], clock_hz)
    rates = {name: v['cycles'] for name, v in issue.items()}
    out: dict = {'cycles': issue}
    for name, group in MIXES.items():
        if name in rates and all(cls in rates for cls in group):
            model = predictions(rates, group)
            closer = min(model, key=lambda m: abs(model[m] - rates[name]))
            out[name] = {'measured': rates[name], **model, 'closer': closer}
    return out
