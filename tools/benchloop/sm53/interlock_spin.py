"""The pixel interlock's ticket wait: NAK's atomic spin against L1-skipping load spins and SM53 clock-backoff probes.

Each pixel adds one to its own counter word inside the interlock, as NAK lowers begin/end_invocation_interlock
(nak_nir_lower_fs_interlock.c; its words in a shipped pack): the warp's highest live lane waits until the ticket word's
top half reaches the warp's ticket, then the live lanes load, add and store their counters, a MEMBAR.GPU, and the
leader adds (ticket & 0xfe00) + 0x200 to the ticket word. The ticket word is the root table's ticket buffer
(c[0][0x260]) + ((SR_AFFINITY & 0xff) * 256 + (ticket & 0xff)) * 4.

`generate()` builds the original `interlock-spin` batch (`atom`, `ldcg`, `ldcv`, `none`, alone and `-compete`).
`generate_clock_probe()` builds the standalone `interlock-clock` silicon probe comparing `CS2R SR_CLOCKLO` (`0x50c8`,
`wr_bar`/`wt_bar_mask` completion safety + latency) against `S2R SR_CLOCKLO` (`0xf0c8`) and plain `LDG.E.CG` under
contention, placing proven-safe `S2R` and fixed-delay arms ahead of unverified `CS2R` scoreboard arms.
"""

import struct
import sys

from tools.benchloop import protocol, ticket
from tools.benchloop.sm53 import encode as e, interlock, latency, operands, program, sph, stats
from tools.benchloop.sm53.memops import SR_CLOCKLO, cs2r, guarded, ldg, red_add, s2r, vote_any

FORMS = ('atom', 'ldcg', 'ldcv', 'none')
SIZES = ((64, 64), (256, 256), (1280, 720))
CHECK_DRAWS = [64]
TIMED_DRAWS = [16, 32, 64]
TIMED_REPEATS, TIMED_WARMUP = 3, 1

CLOCK_DELAY, CLOCK_START, CLOCK_NOW, CLOCK_DIFF = 24, 25, 26, 27
CLOCK_DONE, CLOCK_ANY = 4, 5
BACKOFF_MIN, BACKOFF_MAX = 32, 256
CLOCK_CHAIN_BODIES = (16, 32, 64)
CLOCK_CHAIN_LOOPS = 128
CLOCK_SPIN_SIZES = ((64, 64), (256, 256))
CLOCK_SPIN_FORMS = ('ldcg', 'ldcg_s2r', 'ldcg_cs2r')


def code(form: str, log_stride: int) -> bytes:
    instrs = interlock.pixel_counter(log_stride) + interlock.alive()
    if form == 'none':
        instrs += interlock.count()
    else:
        instrs += interlock.leader_and_ticket()
        instrs += interlock.wait(form, len(instrs))
        instrs += interlock.count()
        instrs += interlock.wait(form, len(instrs))
        release = guarded(red_add(interlock.TICKET_WORD, interlock.INCREMENT), interlock.LEADER)
        instrs.append(release.sched(rd_bar=2, delay=e.BARRIER_SET_CYCLES))
    instrs += [program.SEPARATOR, *program.epilogue((interlock.VALUE, interlock.VALUE))]
    return e.bundle(e.finish(instrs))


def experiment(form: str, width: int, height: int, timed: bool, compete: bool) -> dict:
    log_stride = (width - 1).bit_length()
    c = code(form, log_stride)
    control = interlock.CONFLICT_DETECT_PIXEL if form != 'none' else 0
    draws = TIMED_DRAWS if timed else CHECK_DRAWS
    repeats, warmup = (TIMED_REPEATS, TIMED_WARMUP) if timed else (1, 0)
    x = {
        'id': f'interlock.{form}.{width}x{height}.{"timed" if timed else "check"}{"-compete" if compete else ""}',
        'kind': 'readback',
        'target': {'format': 'rgba32float', 'width': width, 'height': height},
        'program': {
            'sph': protocol.b64(sph.pixel(operands.attributes(c), reads_ticket=ticket.reads_ticket(c))),
            'code': protocol.b64(c),
            'data': protocol.b64(bytes(16)),
            'gprs': interlock.GPRS,
        },
        'draws': draws,
        'repeats': repeats,
        'warmup': warmup,
        'readback': 'none',
        'methods': [[interlock.INTERLOCK_CONTROL, control]],
        'restore': [[interlock.INTERLOCK_CONTROL, 0]],
        'storage': {'bytes': (height << log_stride) * 4, 'va_at': 0, 'readback': 'summary'},
        'meta': {
            'family': 'interlock',
            'form': form,
            'size': [width, height],
            'stride': 1 << log_stride,
            'expect': sum(draws) * (repeats + warmup),
            'compete': compete,
        },
    }
    if compete:
        x['compete'] = True
    protocol.validate_experiment(x)
    return x


def generate() -> list[dict]:
    """Give the checks and the timed runs, alone and beside a second channel, smallest target first.

    A 720p run that spins too long goes last.
    """
    out = [latency.warm_up('interlock.warm')]
    for width, height in SIZES:
        for compete in (False, True):
            out += [experiment(form, width, height, timed, compete) for timed in (False, True) for form in FORMS]
    return out


def analyse(experiments: list[dict], results: dict[str, dict]) -> dict:
    """Give per experiment the pixels whose counter missed the draws run, and the GPU time a draw."""
    out = {}
    for x in experiments:
        if x['meta'].get('family') != 'interlock':
            continue
        r = results.get(x['id']) or {}
        row = _storage_row(x, r)
        if x['meta']['compete']:
            row['competitor_buffers'] = (r.get('competitor') or {}).get('buffers')
        out[x['id']] = row
    return out


def _storage_row(x: dict, r: dict) -> dict:
    width, height = x['meta']['size']
    row: dict = {'status': r.get('status', 'missing'), 'why': r.get('why', '')}
    words = protocol.storage_words(r)
    if row['status'] == 'ok' and words is not None:
        gaps = (x['meta']['stride'] - width) * height
        row['lost'] = width * height - words.get(x['meta']['expect'], 0)
        row['other_values'] = {k: n for k, n in words.items() if k not in (x['meta']['expect'], 0)}
        row['zeros_past_gaps'] = words.get(0, 0) - gaps
    row['ns_per_draw'] = per_draw(r)
    return row


def per_draw(result: dict) -> float | None:
    points = [(run['draws'], stats.median(run['gpu_ns'])) for run in result.get('runs', []) if run.get('gpu_ns')]
    return stats.line(*zip(*points, strict=True)).slope if len({d for d, _ in points}) > 1 else None


def clock_read(op: str, dst: int) -> e.Instr:
    if op == 's2r':
        return s2r(dst, SR_CLOCKLO)
    if op == 'cs2r':
        return cs2r(dst, SR_CLOCKLO)
    raise ValueError(f'unknown clock op {op!r}')


def clock_safety_program(op: str, wait_bar: bool) -> program.Program:
    """Read SR_CLOCKLO twice into R8, R9 and return (t0, t1 - t0) via scoreboard wait or fixed delay."""
    if wait_bar:
        reads = [
            clock_read(op, 8).sched(wr_bar=0, delay=e.BARRIER_SET_CYCLES),
            e.nop().sched(wait=1, delay=1),
            clock_read(op, 9).sched(wr_bar=0, delay=e.BARRIER_SET_CYCLES),
            e.nop().sched(wait=1, delay=1),
        ]
    else:
        reads = [
            clock_read(op, 8).sched(delay=program.SAFE_DELAY),
            e.nop().sched(delay=program.SAFE_DELAY),
            clock_read(op, 9).sched(delay=program.SAFE_DELAY),
            e.nop().sched(delay=program.SAFE_DELAY),
        ]
    body = [*reads, e.iadd(10, 9, 8, 'neg_b').sched(delay=program.SAFE_DELAY)]
    return program.fragment(body, {}, (8, 10))


def clock_latency_program(op: str, wait_bar: bool, body_n: int, loops: int = CLOCK_CHAIN_LOOPS) -> program.Program:
    """Run `body_n * loops` chained SR_CLOCKLO reads in one warp via alternating barriers or fixed delay."""
    if wait_bar:
        body = [
            clock_read(op, 8).sched(wr_bar=j % 2, wait=1 << (1 - j % 2), delay=e.BARRIER_SET_CYCLES)
            for j in range(body_n)
        ]
    else:
        body = [clock_read(op, 8).sched(delay=program.FIXED_LATENCY) for _ in range(body_n)]
    return program.fragment(body, {}, (8, 8), loop=loops)


def wait_backoff(clock_op: str, top: int) -> list[e.Instr]:
    """Ticket wait on LDG.E.CG with fast-path exit ahead of exponential clock backoff (32..256 cycles)."""
    init_delay = e.mov32i(CLOCK_DELAY, BACKOFF_MIN).sched(delay=interlock.SLOW)
    outer_top = top + 1
    poll = [
        guarded(ldg(interlock.SEEN, interlock.TICKET_WORD, 'cg'), interlock.LEADER).sched(
            wr_bar=0, delay=e.BARRIER_SET_CYCLES
        ),
        e.shr(interlock.SEEN, interlock.SEEN, e.Imm(16)).sched(wait=1, delay=program.FIXED_LATENCY),
        e.isetp(interlock.MATCH, interlock.SEEN, interlock.EXPECTED, 'eq and', interlock.LEADER).sched(
            delay=interlock.PRED_LATENCY
        ),
        vote_any(e.RZ, interlock.ANY, interlock.MATCH).sched(delay=interlock.PRED_LATENCY),
    ]
    exit_at = outer_top + len(poll)
    start_reads = [
        clock_read(clock_op, CLOCK_START).sched(wr_bar=0, delay=e.BARRIER_SET_CYCLES),
        e.nop().sched(wait=1, delay=1),
    ]
    inner_top = exit_at + 1 + len(start_reads)
    inner_body = [
        clock_read(clock_op, CLOCK_NOW).sched(wr_bar=0, delay=e.BARRIER_SET_CYCLES),
        e.nop().sched(wait=1, delay=1),
        e.iadd(CLOCK_DIFF, CLOCK_NOW, CLOCK_START, 'neg_b').sched(delay=program.FIXED_LATENCY),
        e.isetp(CLOCK_DONE, CLOCK_DIFF, CLOCK_DELAY, 'ge and', interlock.LEADER).sched(delay=interlock.PRED_LATENCY),
        vote_any(e.RZ, CLOCK_ANY, CLOCK_DONE).sched(delay=interlock.PRED_LATENCY),
    ]
    inner_bra_at = inner_top + len(inner_body)
    inner_bra = e.bra(e.address(inner_top) - e.address(inner_bra_at) - 8, CLOCK_ANY, neg=True).sched(
        delay=program.BRANCH_DELAY
    )
    step = [
        e.shl(CLOCK_DELAY, CLOCK_DELAY, e.Imm(1)).sched(delay=program.FIXED_LATENCY),
        e.imnmx(CLOCK_DELAY, CLOCK_DELAY, e.Imm(BACKOFF_MAX), 'min').sched(delay=program.FIXED_LATENCY),
    ]
    outer_bra_at = inner_bra_at + 1 + len(step)
    outer_bra = e.bra(e.address(outer_top) - e.address(outer_bra_at) - 8).sched(delay=program.BRANCH_DELAY)
    after_loop = outer_bra_at + 1
    fast_exit = e.bra(e.address(after_loop) - e.address(exit_at) - 8, interlock.ANY).sched(delay=program.BRANCH_DELAY)
    return [init_delay, *poll, fast_exit, *start_reads, *inner_body, inner_bra, *step, outer_bra]


def backoff_code(form: str, log_stride: int) -> bytes:
    """Single-wait interlock counter shader comparing plain LDG.E.CG against S2R/CS2R clock backoff."""
    if form not in CLOCK_SPIN_FORMS:
        raise ValueError(f'unknown backoff spin form {form!r}')
    instrs = interlock.pixel_counter(log_stride) + interlock.alive() + interlock.leader_and_ticket()
    if form == 'ldcg':
        instrs += interlock.wait('ldcg', len(instrs))
    else:
        instrs += wait_backoff(form.removeprefix('ldcg_'), len(instrs))
    instrs += interlock.count()
    release = guarded(red_add(interlock.TICKET_WORD, interlock.INCREMENT), interlock.LEADER)
    instrs.append(release.sched(rd_bar=2, delay=e.BARRIER_SET_CYCLES))
    instrs += [program.SEPARATOR, *program.epilogue((interlock.VALUE, interlock.VALUE))]
    return e.bundle(e.finish(instrs))


def _clock_safety_experiment(name: str, op: str, wait_bar: bool) -> dict:
    prog = clock_safety_program(op, wait_bar)
    x = {
        'id': f'interlock_clock.safety.{name}',
        'kind': 'fragment',
        'target': dict(latency.TARGET),
        'rect': list(latency.ONE_WARP),
        'draws': [1],
        'repeats': 1,
        'warmup': 0,
        'clear': [0, 0, 0, 0],
        'readback': 'summary',
        'program': prog.wire(),
        'meta': {'family': 'interlock-clock', 'probe': 'safety', 'arm': name, 'op': op, 'wait_bar': wait_bar},
    }
    protocol.validate_experiment(x)
    return x


def _clock_latency_experiment(name: str, op: str, wait_bar: bool, body_n: int) -> dict:
    k = body_n * CLOCK_CHAIN_LOOPS
    prog = clock_latency_program(op, wait_bar, body_n)
    x = {
        'id': f'interlock_clock.latency.{name}.k{k:04d}',
        'kind': 'fragment',
        'target': dict(latency.TARGET),
        'rect': list(latency.ONE_WARP),
        'draws': list(latency.DRAWS),
        'repeats': latency.REPEATS,
        'warmup': latency.WARMUP,
        'clear': [0, 0, 0, 0],
        'readback': 'summary',
        'program': prog.wire(),
        'meta': {'family': 'interlock-clock', 'probe': 'latency', 'arm': name, 'op': op, 'wait_bar': wait_bar, 'k': k},
    }
    protocol.validate_experiment(x)
    return x


def _clock_spin_experiment(form: str, width: int, height: int, timed: bool) -> dict:
    log_stride = (width - 1).bit_length()
    c = backoff_code(form, log_stride)
    draws = TIMED_DRAWS if timed else CHECK_DRAWS
    repeats, warmup = (TIMED_REPEATS, TIMED_WARMUP) if timed else (1, 0)
    x = {
        'id': f'interlock_clock.spin.{form}.{width}x{height}.{"timed" if timed else "check"}',
        'kind': 'readback',
        'target': {'format': 'rgba32float', 'width': width, 'height': height},
        'program': {
            'sph': protocol.b64(sph.pixel(operands.attributes(c), reads_ticket=ticket.reads_ticket(c))),
            'code': protocol.b64(c),
            'data': protocol.b64(bytes(16)),
            'gprs': interlock.GPRS,
        },
        'draws': draws,
        'repeats': repeats,
        'warmup': warmup,
        'readback': 'none',
        'methods': [[interlock.INTERLOCK_CONTROL, interlock.CONFLICT_DETECT_PIXEL]],
        'restore': [[interlock.INTERLOCK_CONTROL, 0]],
        'storage': {'bytes': (height << log_stride) * 4, 'va_at': 0, 'readback': 'summary'},
        'meta': {
            'family': 'interlock-clock',
            'probe': 'spin',
            'form': form,
            'size': [width, height],
            'stride': 1 << log_stride,
            'expect': sum(draws) * (repeats + warmup),
        },
    }
    protocol.validate_experiment(x)
    return x


def generate_clock_probe() -> list[dict]:
    """Build the standalone GM20B CS2R silicon probe, ordering proven-safe S2R/LDG.E.CG arms ahead of CS2R wr_bar."""
    out = [
        latency.warm_up('interlock_clock.warm'),
        _clock_safety_experiment('s2r_bar', 's2r', True),
        _clock_safety_experiment('cs2r_delay', 'cs2r', False),
    ]
    for name, op, wait_bar in (('s2r_bar', 's2r', True), ('cs2r_delay', 'cs2r', False)):
        out += [_clock_latency_experiment(name, op, wait_bar, n) for n in CLOCK_CHAIN_BODIES]
    for width, height in CLOCK_SPIN_SIZES:
        for form in ('ldcg', 'ldcg_s2r'):
            out += [_clock_spin_experiment(form, width, height, timed) for timed in (False, True)]

    out.append(_clock_safety_experiment('cs2r_bar', 'cs2r', True))
    out += [_clock_latency_experiment('cs2r_bar', 'cs2r', True, n) for n in CLOCK_CHAIN_BODIES]
    for width, height in CLOCK_SPIN_SIZES:
        out += [_clock_spin_experiment('ldcg_cs2r', width, height, timed) for timed in (False, True)]
    return out


def _first_nonzero_texel(result: dict) -> tuple[int, int] | None:
    for entry in (result.get('texels') or {}).get('distinct') or []:
        words = list(struct.unpack('<4I', bytes.fromhex(entry['value'])))
        if any(words):
            return program.unpack(words)
    return None


def analyse_clock_probe(experiments: list[dict], results: dict[str, dict], clock_hz: float = 769.1e6) -> dict:
    """Analyse the standalone CS2R vs S2R vs LDG.E.CG silicon probe."""
    safety: dict[str, dict] = {}
    latency_points: dict[str, list[tuple[int, float]]] = {}
    latency_status: dict[str, list[str]] = {}
    spin: dict[str, dict] = {}
    for x in experiments:
        meta = x.get('meta') or {}
        if meta.get('family') != 'interlock-clock':
            continue
        r = results.get(x['id']) or {}
        probe = meta.get('probe')
        if probe == 'safety':
            status = r.get('status', 'missing')
            row: dict = {'status': status, 'why': r.get('why', '')}
            unpacked = _first_nonzero_texel(r) if status == 'ok' else None
            if unpacked is not None:
                t0, dt = unpacked
                row.update(t0=t0, dt=dt, monotonic=0 < dt < (1 << 31))
            else:
                row['monotonic'] = False
            safety[meta['arm']] = row
        elif probe == 'latency':
            arm = meta['arm']
            latency_status.setdefault(arm, []).append(r.get('status', 'missing'))
            slope = per_draw(r)
            if r.get('status') == 'ok' and slope is not None:
                latency_points.setdefault(arm, []).append((meta['k'], slope))
        elif probe == 'spin':
            spin[x['id']] = _storage_row(x, r)

    lat: dict[str, dict] = {}
    for arm, statuses in latency_status.items():
        pts = latency_points.get(arm, [])
        if len(pts) >= 2 and all(s == 'ok' for s in statuses):
            ns_per_op = stats.line(*zip(*pts, strict=True)).slope
            lat[arm] = {
                'status': 'ok',
                'ns_per_op': ns_per_op,
                'cycles_per_op': ns_per_op * clock_hz / 1e9,
            }
        else:
            lat[arm] = {'status': next((s for s in statuses if s != 'ok'), 'missing'), 'cycles_per_op': None}

    return {'safety': safety, 'latency': lat, 'spin': spin}


if __name__ == '__main__':
    sys.stdout.buffer.write(protocol.encode_batch(protocol.batch('interlock-clock', generate_clock_probe())))
