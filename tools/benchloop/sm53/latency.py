"""The latency, throughput, dual-issue and operand-reuse experiments, as protocol experiment dicts.

ladder      dependent FADDs at delays 6..15 in one warp, a short body looped: time against k * delay gives the clock.
stall       dependent chains of each fixed-latency op at delays 1..15: the smallest delay whose texel is right is
            the op's latency (fixed-latency results are not interlocked, so a shorter delay reads a stale register).
chain       k dependent variable-latency ops through alternating barriers, a body of n looped a fixed number of times:
            time against k gives the latency (each link issued two cycles after the last, the soonest a barrier it
            set can be waited on).
throughput  independent streams of each op over the whole target: time against k gives issue cycles per warp.
dual        independent pairs (X, Y) in one warp, X at delay 0 and at delay 1, a short body looped.
reuse       FFMA streams whose operands share a register bank, with and without reuse flags, and spread over banks.

Each experiment carries a host-only `meta` (what fit.py groups by) and `expect` (the interpreter's texel).
"""

from collections.abc import Callable, Iterable
from dataclasses import dataclass

from tools.benchloop import protocol
from tools.benchloop.sm53 import encode as e, operands, program
from tools.benchloop.sm53.encode import PT, f32
from tools.benchloop.sm53.ops import (
    CHAIN0,
    FIXED,
    K0,
    K1,
    K2,
    PAIR_FORMS,
    PAIRS,
    SRC,
    VARIABLE_OPS,
    Recipe,
    chain_registers,
)
from tools.benchloop.sm53.program import SEPARATOR, Program

FAMILIES = ('ladder', 'stall', 'chain', 'throughput', 'dual', 'reuse')
WIDTH, HEIGHT = 256, 128
TARGET = {'format': 'rgba32float', 'width': WIDTH, 'height': HEIGHT}
ONE_WARP = [0, 0, 8, 4]
WHOLE = [0, 0, WIDTH, HEIGHT]
DRAWS = [1, 2, 4, 8]
REPEATS = 5
WARMUP = 2
LADDER_DELAYS = range(6, 16)
# A lone warp's command buffer costs ~45 us of fixed time with ~2 us of noise, and straight-line code past a few KiB
# runs at instruction-fetch speed (~16 cycles an instruction): timed chains are a short body looped many times
LADDER_BODY = 32
LADDER_LOOPS = 256
STALL_DELAYS = range(1, 16)
STALL_K = 24
PRED_LINKS = 6
CHAIN_BODIES = (16, 32, 64)
CHAIN_LOOPS = 128
THROUGHPUT_KS = (512, 1024, 2048)
STREAMS = {'gpr': 16, 'variable': 8, 'pred': 6}
DUAL_BODIES = (16, 32, 64)
DUAL_LOOPS = 128
REUSE_KS = (512, 1024)
REUSE_ACCUMULATORS = 8
# operand A, operand B, reuse flags: 'conflict' reads three bank-0 registers, 'reuse' keeps A and B cached
REUSE_VARIANTS = {
    'spread': (K1, K2, 0),
    'conflict': (K0, CHAIN0, 0),
    'reuse': (K0, CHAIN0, 0b0011),
    'reuse-1': (K0, CHAIN0, 0b0001),
    'reuse-2': (K0, CHAIN0, 0b0010),
    'reuse-4': (K0, CHAIN0, 0b0100),
    'reuse-8': (K0, CHAIN0, 0b1000),
    'reuse-c': (K0, CHAIN0, 0b1100),
    'reuse-f': (K0, CHAIN0, 0b1111),
}


def experiment(eid: str, prog: Program, rect: list[int], meta: dict, exact: bool = True) -> dict:
    texel = program.expected_texel(prog.code, prog.data)
    out = {
        'id': eid,
        'kind': 'fragment',
        'target': dict(TARGET),
        'rect': list(rect),
        'draws': list(DRAWS),
        'repeats': REPEATS,
        'warmup': WARMUP,
        'clear': [0, 0, 0, 0],
        'readback': 'summary',
        'program': prog.wire(),
        'meta': meta,
        'expect': {'texel': texel, 'outside': [0, 0, 0, 0], 'exact': exact},
    }
    protocol.validate_experiment(out)
    return out


def ladder() -> list[dict]:
    out = []
    for d in LADDER_DELAYS:
        body = [e.fadd(CHAIN0, CHAIN0, K1).sched(delay=d) for _ in range(LADDER_BODY)]
        prog = program.fragment(body, {CHAIN0: f32(0.0), K1: f32(1.0)}, (CHAIN0, CHAIN0), loop=LADDER_LOOPS)
        meta = {'family': 'ladder', 'op': 'FADD', 'k': LADDER_BODY * LADDER_LOOPS, 'delay': d}
        out.append(experiment(f'ladder.d{d:02d}', prog, ONE_WARP, meta))
    return out


@dataclass(frozen=True)
class Stall:
    """A stall chain's parts, drawn (fragment: the results as a texel) or dispatched (kernel: stored to its output)."""

    body: list
    init: dict[int, int]
    results: tuple[int, int]
    preds: dict[int, bool] | None = None

    def fragment(self) -> Program:
        return program.fragment(self.body, self.init, self.results, self.preds)

    def kernel(self) -> Program:
        return program.compute_out(self.body, self.init, self.results, self.preds)

    def expected(self) -> tuple[int, int]:
        """Give the two results the interpreter says the chain ends with (its fragment form's texel, unpacked)."""
        p = self.fragment()
        return program.unpack(program.expected_texel(p.code, p.data))


def _stall_gprs(r: Recipe, delay: int) -> Stall:
    regs = chain_registers(STALL_K + 1)
    body = [r.link(regs[j + 1], regs[j]).sched(delay=delay) for j in range(STALL_K)]
    init = {regs[0]: r.seed} | {regs[j]: r.poison + r.step * j for j in range(1, STALL_K + 1)} | r.constants()
    return Stall(body, init, (regs[-1], regs[STALL_K // 2]))


def _stall_preds(r: Recipe, delay: int) -> Stall:
    """Give a chain P0 -> P6 of `p = (K1 < K2) xor accum`, each P starting as the complement of its true value."""
    truth = [bool(r.seed)]
    for _ in range(PRED_LINKS):
        truth.append(not truth[-1])
    preds = {0: truth[0]} | {j: not truth[j] for j in range(1, PRED_LINKS + 1)}
    body = [r.link(j + 1, j).sched(delay=delay) for j in range(PRED_LINKS)]
    body += [SEPARATOR, e.sel(CHAIN0, K1, K2, PRED_LINKS), e.sel(CHAIN0 + 4, K1, K2, PRED_LINKS // 2)]
    return Stall(body, r.constants(), (CHAIN0, CHAIN0 + 4), preds)


def stall(recipes: Iterable[Recipe] = FIXED) -> list[dict]:
    out = []
    for r in recipes:
        build = _stall_preds if r.pred else _stall_gprs
        for d in STALL_DELAYS:
            meta = {'family': 'stall', 'op': r.key, 'nak': r.nak, 'delay': d, 'pred': r.pred}
            e = experiment(f'stall.{r.slug}.d{d:02d}', build(r, d).fragment(), ONE_WARP, meta)
            # the texel is the answer: one draw, once
            out.append(e | {'draws': [1], 'repeats': 1, 'warmup': 0})
    return out


def compute_stall(recipes: Iterable[Recipe] = FIXED, threads: int = 32) -> list[dict]:
    """Give the stall family as kernels storing their results: one warp, no target, the results as buffer words."""
    out = []
    for r in recipes:
        build = _stall_preds if r.pred else _stall_gprs
        for d in STALL_DELAYS:
            chain = build(r, d)
            k = chain.kernel()
            x, y = chain.expected()
            compute = k.wire() | {'threads': [threads, 1, 1], 'groups': [1, 1, 1]}
            out.append(
                {
                    'id': f'cstall.{r.slug}.d{d:02d}',
                    'kind': 'compute',
                    'target': {'format': 'rgba32float', 'width': 1, 'height': 1},
                    'compute': compute | {'output': threads * program.OUTPUT_STRIDE},
                    'readback': 'full',
                    'meta': {'family': 'compute-stall', 'op': r.key, 'nak': r.nak, 'delay': d, 'pred': r.pred},
                    'expect': {'words': [x, y] * threads},
                }
            )
            protocol.validate_experiment(out[-1])
    return out


def _chain_link(r: Recipe, j: int) -> e.Instr:
    """Give link j: it sets barrier j % 2 and waits on the other, which the link before set (or last iteration's)."""
    return r.link(CHAIN0, CHAIN0).sched(wr_bar=j % 2, wait=1 << (1 - j % 2), delay=e.BARRIER_SET_CYCLES)


def chain(recipes: Iterable[Recipe] = VARIABLE_OPS) -> list[dict]:
    out = []
    for r in recipes:
        for n in CHAIN_BODIES:
            k = n * CHAIN_LOOPS
            body = [_chain_link(r, j) for j in range(n)]
            prog = program.fragment(body, {CHAIN0: r.seed} | r.constants(), (CHAIN0, CHAIN0), loop=CHAIN_LOOPS)
            meta = {'family': 'chain', 'op': r.key, 'nak': r.nak, 'k': k}
            out.append(experiment(f'chain.{r.slug}.k{k:03d}', prog, ONE_WARP, meta, r.exact))
    return out


def _streams(r: Recipe, k: int) -> Program:
    """Give k instructions of `r` over independent streams: accumulators, or fresh results of SRC for barrier ops."""
    if r.pred:
        body = [r.link(i % STREAMS['pred'], PT) for i in range(k)]
        last = STREAMS['pred'] - 1
        body += [SEPARATOR, e.sel(CHAIN0, K1, K2, 0), e.sel(CHAIN0 + 4, K1, K2, last)]
        return program.fragment(body, r.constants(), (CHAIN0, CHAIN0 + 4))
    if r.fixed:
        regs = chain_registers(STREAMS['gpr'])
        body = [r.link(regs[i % len(regs)], regs[i % len(regs)]) for i in range(k)]
        init = dict.fromkeys(regs, r.seed)
    else:
        regs = chain_registers(STREAMS['variable'])
        body = [r.link(regs[i % len(regs)], SRC).sched(wr_bar=0) for i in range(k)]
        init = dict.fromkeys(regs, 0) | {SRC: r.seed}
    return program.fragment(body, init | r.constants(), (regs[0], regs[-1]))


def throughput(recipes: Iterable[Recipe] = FIXED + VARIABLE_OPS) -> list[dict]:
    out = []
    for r in recipes:
        for k in THROUGHPUT_KS:
            meta = {'family': 'throughput', 'op': r.key, 'nak': r.nak, 'k': k}
            out.append(experiment(f'tput.{r.slug}.k{k:03d}', _streams(r, k), WHOLE, meta, r.exact))
    return out


def _pair_form(op: str, d: int) -> e.Instr:
    instr = PAIR_FORMS[op](d)
    return instr if operands.of(instr).fixed else instr.sched(wr_bar=0)


def dual(pairs: Iterable[tuple[str, str]] = PAIRS) -> list[dict]:
    out = []
    init = {K1: f32(1.5), K2: f32(2.5), SRC: f32(0.5), CHAIN0: 0, CHAIN0 + 4: 0}
    for x, y in pairs:
        first, second = _pair_form(x, CHAIN0), _pair_form(y, CHAIN0 + 4).sched(delay=1)
        for delay in (0, 1):
            for n in DUAL_BODIES:
                k = n * DUAL_LOOPS
                body = [first.sched(delay=delay), second] * n
                prog = program.fragment(body, init, (CHAIN0, CHAIN0 + 4), loop=DUAL_LOOPS)
                meta = {'family': 'dual', 'op': f'{x}+{y}', 'pair': [x, y], 'delay': delay, 'k': k}
                eid = f'dual.{x.lower()}-{y.lower()}.d{delay}.k{k:03d}'
                out.append(experiment(eid, prog, ONE_WARP, meta, exact=x != 'MUFU' and y != 'MUFU'))
    return out


def reuse() -> list[dict]:
    out = []
    accumulators = [CHAIN0 + 4 * (i + 1) for i in range(REUSE_ACCUMULATORS)]
    for variant, (a, b, flags) in REUSE_VARIANTS.items():
        for k in REUSE_KS:
            accs = [accumulators[i % REUSE_ACCUMULATORS] for i in range(k)]
            body = [e.ffma(acc, a, b, acc).sched(reuse=flags) for acc in accs]
            init = {a: f32(1.0), b: f32(0.5)} | dict.fromkeys(accumulators, f32(0.0))
            prog = program.fragment(body, init, (accumulators[0], accumulators[-1]))
            meta = {'family': 'reuse', 'op': 'FFMA', 'variant': variant, 'k': k, 'reuse': flags}
            out.append(experiment(f'reuse.{variant}.k{k:03d}', prog, WHOLE, meta))
    return out


def generate(
    families: Iterable[str] = FAMILIES,
    ops: Iterable[str] | None = None,
    pairs: Iterable[tuple[str, str]] = PAIRS,
) -> list[dict]:
    """Give the chosen families' experiments, the ladder first; `ops` limits the per-op families to those keys."""
    chosen = None if ops is None else set(ops)

    def pick(recipes: tuple[Recipe, ...]) -> list[Recipe]:
        return [r for r in recipes if chosen is None or r.key in chosen]

    builders: dict[str, Callable[[], list[dict]]] = {
        'ladder': ladder,
        'stall': lambda: stall(pick(FIXED)),
        'chain': lambda: chain(pick(VARIABLE_OPS)),
        'throughput': lambda: throughput(pick(FIXED + VARIABLE_OPS)),
        'dual': lambda: dual(pairs),
        'reuse': reuse,
    }
    wanted = set(families)
    unknown = wanted - set(FAMILIES)
    if unknown:
        raise ValueError(f'unknown families {sorted(unknown)}')
    return [x for f in FAMILIES if f in wanted for x in builders[f]()]


def batches(experiments: list[dict], size: int = protocol.MAX_EXPERIMENTS) -> list[list[dict]]:
    """Give `experiments` in order, at most `size` a batch."""
    return [experiments[i : i + size] for i in range(0, len(experiments), size)]
