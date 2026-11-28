"""Dual issue timed inside one warp: SR_CLOCKLO read before and after a looped body of independent pairs (X, Y).

latency.dual timed one-warp draws by the command buffer, but consecutive draws run side by side on both SMs and every
scheduler, so its ns per draw was a throughput (8,192 stall-1 pairs, at least 16,384 issue cycles, took 8.35 us at
768 MHz on 11 Nov) and its delay-0 rows fitted under a tenth of a cycle a pair. Here the warp times itself: the texel
is the cycle count of the loop, and the slope over the body's length is cycles a pair, the loop's own branch and
counter left in the intercept. X at delay 0 issues with Y when the pair dual-issues; bit 4 set and clear on both.
"""

from tools.benchloop import protocol
from tools.benchloop.sm53 import encode as e, latency, operands, program, sph
from tools.benchloop.sm53.memops import SR_CLOCKLO, s2r
from tools.benchloop.sm53.ops import CHAIN0, K1, K2, PAIRS, SRC

BODIES = (16, 32, 64)
LOOPS = 16
T0, T1, DT = 40, 41, 42
# with PAIRS' FFMA+MUFU and FFMA+I2F; HFMA2+F2F and FSETP+F2F are NVIDIA's commonest stall-0 pairs
MIO_PAIRS = (('FFMA', 'F2F'), ('FFMA', 'LDC'), ('IADD', 'F2F'), ('HFMA2', 'F2F'), ('FSETP', 'F2F'))


def timed(body: list[e.Instr], init: dict[int, int]) -> program.Program:
    """Give `body` looped LOOPS times between two clock reads; the texel is (cycles, t0)."""
    start = [s2r(T0, SR_CLOCKLO).sched(wr_bar=0, delay=e.BARRIER_SET_CYCLES), e.nop().sched(wait=1, delay=1)]
    head = program.prologue(init)
    loop = program.looped(body, LOOPS, head + start)
    end = [
        program.SEPARATOR,
        s2r(T1, SR_CLOCKLO).sched(wr_bar=0, delay=e.BARRIER_SET_CYCLES),
        e.nop().sched(wait=1, delay=1),
        e.iadd(DT, T1, T0, 'neg_b').sched(delay=program.SAFE_DELAY),
    ]
    code = e.bundle(e.finish([*head, *start, *loop, *end, program.SEPARATOR, *program.epilogue((DT, T0))]))
    return program.Program(code, program.data_of(init), program._gprs(code), sph.pixel(operands.attributes(code)))


def experiment(x: str, y: str, delay: int, yld: bool, n: int) -> dict:
    first = latency._pair_form(x, CHAIN0).sched(delay=delay, yld=yld)
    second = latency._pair_form(y, CHAIN0 + 4).sched(delay=1, yld=yld)
    init = {K1: e.f32(1.5), K2: e.f32(2.5), SRC: e.f32(0.5), CHAIN0: 0, CHAIN0 + 4: 0}
    prog = timed([first, second] * n, init)
    tag = '.y' if yld else ''
    out = {
        'id': f'dualc.{x.lower()}-{y.lower()}{tag}.d{delay}.n{n:02d}',
        'kind': 'fragment',
        'target': dict(latency.TARGET),
        'rect': list(latency.ONE_WARP),
        'draws': [1],
        'repeats': 3,
        'warmup': 1,
        'clear': [0, 0, 0, 0],
        'readback': 'summary',
        'program': prog.wire(),
        'meta': {'family': 'dual-clock', 'op': f'{x}+{y}', 'delay': delay, 'yld': yld, 'pairs': n * LOOPS},
    }
    protocol.validate_experiment(out)
    return out


def generate() -> list[dict]:
    return [
        latency.warm_up('dualc.warm'),
        *(
            experiment(x, y, delay, yld, n)
            for x, y in PAIRS + MIO_PAIRS
            for yld in (False, True)
            for delay in (0, 1)
            for n in BODIES
        ),
    ]


def cycles(result: dict) -> int | None:
    """Give the loop's cycle count from the texel (the first lane's; every lane of the warp read the same clock)."""
    for entry in (result.get('texels') or {}).get('distinct') or []:
        words = [int.from_bytes(bytes.fromhex(entry['value'])[i : i + 4], 'little') for i in range(0, 16, 4)]
        if any(words):
            return program.unpack(words)[0]
    return None


def analyse(experiments: list[dict], results: dict[str, dict]) -> dict:
    """Give cycles a pair (the slope over the body's length) for each pair, delay and bit-4 setting."""
    points: dict[tuple[str, bool, int], list[tuple[int, int]]] = {}
    for x in experiments:
        meta = x['meta']
        if meta.get('family') != 'dual-clock':
            continue
        c = cycles(results.get(x['id']) or {})
        if c is not None:
            points.setdefault((meta['op'], meta['yld'], meta['delay']), []).append((meta['pairs'], c))
    out: dict = {}
    for (op, yld, delay), pts in sorted(points.items()):
        if len({k for k, _ in pts}) < 2:
            continue
        n = len(pts)
        mk, mc = sum(k for k, _ in pts) / n, sum(c for _, c in pts) / n
        slope = sum((k - mk) * (c - mc) for k, c in pts) / sum((k - mk) ** 2 for k, _ in pts)
        out.setdefault(f'{op}{"/y" if yld else ""}', {})[f'd{delay}'] = round(slope, 3)
    return out
