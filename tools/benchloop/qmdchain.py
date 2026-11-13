"""Batch qmdchain1: the compute class's dependent QMDs, the GPU-side way for one dispatch to start the next.

Only the experiment's own QMD is launched (hw_test_app/bench switch_bench.h chain). Each QMD the chain lists after it
sits in a buffer of the bench's, and QMD i names QMD i + 1 dependent where `schedule` says so: DEPENDENT_QMD_POINTER,
SCHEDULE_ENABLE, TYPE GRID and FIELD_COPY, NAK's QMD 1.7 form (qmd.rs set_dependent_qmd_schedule_pre_v02_04), which
its hardware test test_qmd_dependencies_simple runs from Kepler B on. Every QMD's RELEASE0 adds 1 to the output's last
word, so the counter says how many ran. One thread a kernel stores its marker into its own output word.

qc-steer asks whether a kernel steers the chain: QMD 0's kernel rewrites QMD 1's DEPENDENT_QMD_POINTER from QMD 2 to
QMD 3 before QMD 1 is scheduled. A GPU-side loop of dispatches without copies needs the scheduler to read the pointer
and the schedule bit from memory when a QMD completes, as written by the GPU.
"""

from tools.benchloop import protocol
from tools.benchloop.sm53 import encode
from tools.benchloop.sm53.encode import Cbuf, Imm, Instr
from tools.benchloop.sm53.program import DATA_BANK, SAFE_DELAY, Program, stg

OUTPUT_BYTES = 64
OUTPUT_WORDS = OUTPUT_BYTES // 4
UNWRITTEN = 0xA5A5A5A5  # switch_bench.cpp kUnwritten in each byte
QMD_BYTES = 256
# QMDV01_07 word 5, DEPENDENT_QMD_POINTER (clb1c0qmd.h: bits 160-191), in a QMD's bytes
POINTER_BYTE = 20
MARKERS = (0xA0, 0xB0, 0xC0, 0xD0)
# registers: the output's address (R0, R1), the chain QMDs' (R2, R3), the marker (R4), the steered pointer (R6, R7)
OUT, QMDS, MARKER, POINTER, HIGH = 0, 2, 4, 6, 7


def _addresses() -> list[Instr]:
    return [
        encode.mov(OUT, Cbuf(DATA_BANK, 0)).sched(delay=1),
        encode.mov(OUT + 1, Cbuf(DATA_BANK, 4)).sched(delay=1),
        encode.mov(QMDS, Cbuf(DATA_BANK, 8)).sched(delay=1),
        encode.mov(QMDS + 1, Cbuf(DATA_BANK, 12)).sched(delay=SAFE_DELAY),
    ]


def steered_pointer(to: int) -> list[Instr]:
    """Give R6 = DEPENDENT_QMD_POINTER of chain QMD `to` (QMD i lies 256 * (i - 1) past the address in R2, R3)."""
    return [
        encode.shr(POINTER, QMDS, Imm(8)).sched(delay=1),
        encode.shl(HIGH, QMDS + 1, Imm(24)).sched(delay=SAFE_DELAY),
        encode.lop(POINTER, POINTER, HIGH, 'or').sched(delay=SAFE_DELAY),
        encode.iadd32i(POINTER, POINTER, to - 1).sched(delay=SAFE_DELAY),
    ]


def kernel(index: int, steer: tuple[int, int] | None = None) -> Program:
    """QMD `index`'s kernel: its marker into output word `index`; with `steer` (QMD a, QMD b), QMD a's dependent
    pointer rewritten to name QMD b."""
    body = [*_addresses(), encode.mov32i(MARKER, MARKERS[index]).sched(delay=SAFE_DELAY)]
    if steer:
        body += steered_pointer(steer[1])
        body.append(stg(QMDS, POINTER, (steer[0] - 1) * QMD_BYTES + POINTER_BYTE).sched(delay=1))
    body.append(stg(OUT, MARKER, 4 * index).sched(delay=1))
    code = encode.bundle(encode.finish(body))
    return Program(code, bytes(16), 8)


def expected(ran: list[int]) -> list[int]:
    """Give the output words when QMDs `ran` ran: their markers, UNWRITTEN elsewhere, the release counter last."""
    words = [UNWRITTEN] * OUTPUT_WORDS
    for i in ran:
        words[i] = MARKERS[i]
    words[-1] = len(ran)
    return words


def _experiment(name: str, kernels: list[Program], schedule: list[bool], ran: list[int], meta: dict) -> dict:
    first, *chain = kernels
    compute = first.wire() | {'threads': [1, 1, 1], 'groups': [1, 1, 1], 'output': OUTPUT_BYTES}
    compute |= {'chain': [k.wire() for k in chain], 'schedule': schedule}
    e = {
        'id': f'qc-{name}',
        'kind': 'compute',
        'target': {'format': 'rgba32float', 'width': 1, 'height': 1},
        'compute': compute,
        'readback': 'full',
        'meta': {'family': 'qmdchain', **meta},
        'expect': {'words': expected(ran)},
    }
    protocol.validate_experiment(e)
    return e


def generate() -> list[dict]:
    plain = [kernel(i) for i in range(4)]
    return [
        _experiment('ab', plain[:2], [True], [0, 1], {'question': 'a dependent QMD runs'}),
        _experiment('ab-off', plain[:2], [False], [0], {'question': 'control: a pointer without SCHEDULE_ENABLE'}),
        _experiment('abc', plain[:3], [True, True], [0, 1, 2], {'question': 'a dependent QMD schedules its own'}),
        _experiment(
            'steer',
            [kernel(0, steer=(1, 3)), *plain[1:]],
            [True, True, False],
            [0, 1, 3],
            {'question': 'the scheduler takes a pointer the GPU wrote', 'otherwise': expected([0, 1, 2])},
        ),
    ]


def analyse(experiments: list[dict], results: dict[str, dict]) -> dict:
    """Give each experiment's verdict: its output against what it expects, and against the other outcome it names."""
    rows = []
    for e in experiments:
        r = results.get(e['id']) or {}
        data = protocol.unb64((r.get('output') or {}).get('data') or '')
        words = [int.from_bytes(data[i : i + 4], 'little') for i in range(0, len(data) - 3, 4)]
        otherwise = e['meta'].get('otherwise')
        rows.append(
            {
                'id': e['id'],
                'status': r.get('status'),
                'expected': words == e['expect']['words'],
                'otherwise': otherwise is not None and words == otherwise,
                'words': [f'{w:#x}' for w in words],
                'why': r.get('why', ''),
            }
        )
    return {'rows': rows}
