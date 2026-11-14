"""The pixel interlock's ticket wait: NAK's atomic spin against a spin on an L1-skipping load, by lost updates and time.

Each pixel adds one to its own counter word inside the interlock, as NAK lowers begin/end_invocation_interlock
(nak_nir.c nak_nir_lower_fs_interlock_intrin; its words in a shipped pack): the warp's highest live lane waits until
the ticket word's top half reaches the warp's ticket, then the live lanes load, add and store their counters, a
MEMBAR.GPU, the same wait again, and the leader adds (ticket & 0xfe00) + 0x200 to the ticket word. The ticket word is
the root table's ticket buffer (c[0][0x260]) + ((SR_AFFINITY & 0xff) * 256 + (ticket & 0xff)) * 4.

Form A spins on ATOM.E.ADD [ticket], 0 (shipping); form B on LD.E.CG [ticket]; 'ldcv' on LD.E.CV [ticket], the load
NAK's load-spin lowering emits (LdCacheOp::CacheInvalidate); 'none' skips the interlock (the control that shows the
test can lose updates). Every draw covers the target, so each counter ends at the draws run.
The instructions are NAK's, the control flow is not: NAK's leader loops alone under SSY/PBK/BRK while the rest of the
warp waits at SYNC, and here the whole warp loops with the read guarded to the leader and the exit voted, so form A's
time is near the shipping sequence's rather than equal to it. Every form shares that loop (interlock.py's).

It uses the counter buffer (`storage`), SET_PIXEL_SHADER_INTERLOCK_CONTROL among the methods, and `compete` for the
runs beside a second channel.
"""

from tools.benchloop import protocol, ticket
from tools.benchloop.sm53 import encode as e, interlock, latency, operands, program, sph, stats
from tools.benchloop.sm53.memops import guarded, red_add

FORMS = ('atom', 'ldcg', 'ldcv', 'none')
SIZES = ((64, 64), (256, 256), (1280, 720))
CHECK_DRAWS = [64]
TIMED_DRAWS = [16, 32, 64]
TIMED_REPEATS, TIMED_WARMUP = 3, 1


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
        width, height = x['meta']['size']
        row: dict = {'status': r.get('status', 'missing'), 'why': r.get('why', '')}
        words = protocol.storage_words(r)
        if row['status'] == 'ok' and words is not None:
            gaps = (x['meta']['stride'] - width) * height
            row['lost'] = width * height - words.get(x['meta']['expect'], 0)
            row['other_values'] = {k: n for k, n in words.items() if k not in (x['meta']['expect'], 0)}
            row['zeros_past_gaps'] = words.get(0, 0) - gaps
        row['ns_per_draw'] = per_draw(r)
        if x['meta']['compete']:
            row['competitor_buffers'] = (r.get('competitor') or {}).get('buffers')
        out[x['id']] = row
    return out


def per_draw(result: dict) -> float | None:
    points = [(run['draws'], stats.median(run['gpu_ns'])) for run in result.get('runs', []) if run.get('gpu_ns')]
    return stats.line(*zip(*points, strict=True)).slope if len({d for d, _ in points}) > 1 else None
