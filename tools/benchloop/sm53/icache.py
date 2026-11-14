"""The instruction cache: one executed path of FFMAs laid out over code footprints from 1 KiB to 62 KiB.

Every program runs SEGMENTS_RUN segments of 92 independent FFMAs; each segment counts down, then branches to the next
of its footprint's `distinct` segments (the last back to the first). Only the number of distinct 1 KiB segments the
path cycles through differs, so the time per pixel against footprint shows where code stops fitting the cache. One
distinct segment is the looped small-body control.
"""

from tools.benchloop import protocol
from tools.benchloop.sm53 import encode as e, latency, measure, program, sph
from tools.benchloop.sm53.encode import RZ, f32
from tools.benchloop.sm53.ops import K1, K2, chain_registers
from tools.benchloop.sm53.program import LOOP_COUNTER, LOOP_PREDICATE, SAFE_DELAY, SEPARATOR, Program

FFMAS = 92
SEGMENT = FFMAS + 4
SEGMENTS_RUN = 128
FOOTPRINTS = (1, 2, 4, 8, 12, 16, 20, 24, 32, 48, 62)
FOOTPRINTS_LARGE = (127,)
ACCUMULATORS = chain_registers(16)
DRAWS = [1, 2, 4]
REPEATS = 3
TARGET = {'format': 'rgba32float', 'width': latency.WIDTH, 'height': latency.HEIGHT}
ROW = ('id', 'status', 'ns_per_draw', 'pixels_ok')


def _branch(at: int, to: int, pred: int = e.PT) -> e.Instr:
    return e.bra(e.address(to) - e.address(at) - 8, pred).sched(delay=program.BRANCH_DELAY)


def _segment(start: int, following: int, leave: int) -> list[e.Instr]:
    """Give one segment at instruction `start`: count down, FFMAS FFMAs, on to `following` while the count lasts."""
    body = [
        e.iadd32i(LOOP_COUNTER, LOOP_COUNTER, -1).sched(delay=program.FIXED_LATENCY),
        e.isetp(LOOP_PREDICATE, LOOP_COUNTER, RZ, 'ne').sched(delay=1),
    ]
    body += [e.ffma(r, r, K1, K2).sched(delay=1) for r in (ACCUMULATORS[i % 16] for i in range(FFMAS))]
    end = start + len(body)
    return [*body, _branch(end, following, LOOP_PREDICATE), _branch(end + 1, leave)]


def code(distinct: int) -> tuple[bytes, bytes]:
    """Give the program cycling through `distinct` segments, and its shader data."""
    init = {K1: f32(1.0), K2: f32(0.5)} | dict.fromkeys(ACCUMULATORS, f32(0.0))
    head = [*program.prologue(init), e.mov32i(LOOP_COUNTER, SEGMENTS_RUN).sched(delay=SAFE_DELAY)]
    head += [e.nop()] * (-len(head) % 3)
    base, leave = len(head), len(head) + distinct * SEGMENT
    segments = []
    for k in range(distinct):
        segments += _segment(base + k * SEGMENT, base + (k + 1) % distinct * SEGMENT, leave)
    tail = [SEPARATOR, *program.epilogue((ACCUMULATORS[0], ACCUMULATORS[15]))]
    return e.bundle(e.finish(head + segments + tail)), program.data_of(init)


def experiment(distinct: int) -> dict:
    c, data = code(distinct)
    prog = Program(c, data, ACCUMULATORS[-1] + 1, sph.pixel())
    texel = program.expected_texel(c, data)
    return {
        'id': f'icache.s{distinct:03d}',
        'kind': 'fragment',
        'target': dict(TARGET),
        'draws': list(DRAWS),
        'repeats': REPEATS,
        'warmup': 1,
        'readback': 'summary',
        'program': prog.wire(),
        'meta': {'family': 'icache', 'segments': distinct, 'code_bytes': len(c), 'executed': executed()},
        'expect': {'texel': texel, 'outside': [0, 0, 0, 0], 'exact': True},
    }


def executed() -> int:
    """Give the instructions each pixel runs in the segments: all but the leaving branch of each one."""
    return SEGMENTS_RUN * (SEGMENT - 1) + 1


def generate() -> list[dict]:
    out = [latency.warm_up('icache.warm')]
    for distinct in FOOTPRINTS:
        x = experiment(distinct)
        protocol.validate_experiment(x)
        out.append(x)
    return out


def generate_large() -> list[dict]:
    """Give the footprints past the 64 KiB of code a program had before benchloop/6."""
    return [latency.warm_up('icache128.warm'), *(experiment(d) for d in FOOTPRINTS_LARGE)]


def analyse(experiments: list[dict], results: dict[str, dict], clock_hz: float) -> list[dict]:
    """Give per footprint the GPU cycles per pixel and per warp-instruction a scheduler, and whether texels match."""
    rows = []
    for x in experiments:
        if x['meta'].get('family') != 'icache':
            continue
        r = results.get(x['id']) or {}
        row = {k: v for k, v in measure.row(x, r or None).items() if k in ROW}
        pixels = latency.WIDTH * latency.HEIGHT
        if row.get('ns_per_draw') is not None:
            cycles = row['ns_per_draw'] * clock_hz * 1e-9
            row['cycles_per_pixel'] = cycles / pixels
            row['cycles_per_warp_instr'] = cycles * 8 / (pixels / 32 * x['meta']['executed'])
        rows.append(row | {'segments': x['meta']['segments'], 'code_kib': x['meta']['code_bytes'] / 1024})
    return rows
