"""The T-v3i counter program with a dependent FFMA body placed before or after its ticket wait.

ATOM.E.ADD waits and RED.E.ADD release use the shipped NAK encodings. As in the device-measured T-v3i batch,
the whole warp loops on the leader's voted result; NAK's SSY/PBK/BRK/SYNC control flow differs. The counter, the
leader's ticket and the waits are interlock_spin.py's as well.
"""

from tools.benchloop.sm53 import encode as e, program
from tools.benchloop.sm53.encode import PT, RZ, Cbuf, Imm
from tools.benchloop.sm53.memops import (
    SR_AFFINITY,
    SR_LANE_ID,
    SR_ORDERING_TICKET,
    SR_THREAD_KILL,
    atom_add,
    carry_in,
    carry_out,
    guarded,
    ldg,
    membar_gpu,
    red_add,
    s2r,
    stg,
    vote_any,
)

ROOT_TICKETS = 0x260
INTERLOCK_CONTROL = 0x1224
CONFLICT_DETECT_PIXEL = 2
SPIN_LOADS = {'ldcg': 'cg', 'ldcv': 'cv'}
GPRS = 32
X, Y, INDEX, COUNTER = 4, 5, 6, 8
LANE, KILL, BALLOT, AFFINITY, TICKET, TICKET_ID, INCREMENT, EXPECTED, TICKET_WORD = 10, 11, 12, 13, 14, 15, 16, 17, 18
SEEN, ZERO, VALUE = 20, 21, 22
ALIVE, LEADER, MATCH, ANY = 0, 1, 2, 3
SLOW = program.SAFE_DELAY
PRED_LATENCY = 13


def pixel_counter(log_stride: int) -> list[e.Instr]:
    """Give COUNTER:COUNTER+1 = the counter buffer (c[1][0]) + ((y << log_stride) + x) * 4."""
    return [
        e.ipa(X, 0x70).sched(wr_bar=0, delay=e.BARRIER_SET_CYCLES),
        e.f2i(X, X, 'u32 rz').sched(wait=1, wr_bar=0, delay=1),
        e.ipa(Y, 0x74).sched(wr_bar=1, delay=e.BARRIER_SET_CYCLES),
        e.f2i(Y, Y, 'u32 rz').sched(wait=2, wr_bar=1, delay=e.BARRIER_SET_CYCLES),
        program.SEPARATOR,
        e.shl(INDEX, Y, Imm(log_stride)).sched(delay=SLOW),
        e.iadd(INDEX, INDEX, X).sched(delay=SLOW),
        e.shl(INDEX, INDEX, Imm(2)).sched(delay=SLOW),
        carry_out(e.iadd(COUNTER, INDEX, Cbuf(program.DATA_BANK, 0))).sched(delay=SLOW),
        carry_in(e.iadd(COUNTER + 1, RZ, Cbuf(program.DATA_BANK, 4))).sched(delay=SLOW),
    ]


def alive() -> list[e.Instr]:
    """Give ALIVE: the lane is no helper (NAK's is_helper_invocation reads SR_THREAD_KILL)."""
    return [
        s2r(KILL, SR_THREAD_KILL).sched(wr_bar=1, delay=e.BARRIER_SET_CYCLES),
        e.isetp(ALIVE, KILL, RZ, 'eq').sched(wait=2, delay=PRED_LATENCY),
    ]


def leader_and_ticket() -> list[e.Instr]:
    """Give LEADER (the highest live lane), the ticket word's address and the release step."""
    return [
        s2r(LANE, SR_LANE_ID).sched(wr_bar=0, delay=e.BARRIER_SET_CYCLES),
        vote_any(BALLOT, PT, ALIVE).sched(delay=SLOW),
        e.flo(BALLOT, BALLOT).sched(wr_bar=1, delay=e.BARRIER_SET_CYCLES),
        e.isetp(LEADER, LANE, BALLOT, 'eq and', ALIVE).sched(wait=3, delay=PRED_LATENCY),
        s2r(AFFINITY, SR_AFFINITY).sched(wr_bar=0, delay=1),
        s2r(TICKET, SR_ORDERING_TICKET).sched(wr_bar=1, delay=e.BARRIER_SET_CYCLES),
        program.SEPARATOR,
        e.lop32i(AFFINITY, AFFINITY, 0xFF, 'and').sched(delay=SLOW),
        e.lop32i(TICKET_ID, TICKET, 0xFF, 'and').sched(delay=SLOW),
        e.shl(AFFINITY, AFFINITY, Imm(8)).sched(delay=SLOW),
        e.iadd(AFFINITY, AFFINITY, TICKET_ID).sched(delay=SLOW),
        e.shl(AFFINITY, AFFINITY, Imm(2)).sched(delay=SLOW),
        e.lop32i(INCREMENT, TICKET, 0xFE00, 'and').sched(delay=SLOW),
        e.iadd32i(INCREMENT, INCREMENT, 0x200).sched(delay=SLOW),
        e.shr(EXPECTED, TICKET, Imm(16)).sched(delay=SLOW),
        e.mov32i(ZERO, 0).sched(delay=SLOW),
        carry_out(e.iadd(TICKET_WORD, AFFINITY, Cbuf(0, ROOT_TICKETS))).sched(delay=SLOW),
        carry_in(e.iadd(TICKET_WORD + 1, RZ, Cbuf(0, ROOT_TICKETS + 4))).sched(delay=SLOW),
    ]


def wait(form: str, top: int) -> list[e.Instr]:
    """Give the leader's spin at instruction `top`: until the ticket word's top half is EXPECTED, the warp waits."""
    read = atom_add(SEEN, TICKET_WORD, ZERO) if form == 'atom' else ldg(SEEN, TICKET_WORD, SPIN_LOADS[form])
    body = [
        guarded(read, LEADER).sched(wr_bar=0, delay=e.BARRIER_SET_CYCLES),
        e.shr(SEEN, SEEN, Imm(16)).sched(wait=1, delay=program.FIXED_LATENCY),
        e.isetp(MATCH, SEEN, EXPECTED, 'eq and', LEADER).sched(delay=PRED_LATENCY),
        vote_any(RZ, ANY, MATCH).sched(delay=PRED_LATENCY),
    ]
    at = top + len(body)
    back = e.bra(e.address(top) - e.address(at) - 8, ANY, neg=True).sched(delay=program.BRANCH_DELAY)
    return [*body, back]


def count() -> list[e.Instr]:
    return [
        guarded(ldg(VALUE, COUNTER, 'cg'), ALIVE).sched(wr_bar=0, delay=e.BARRIER_SET_CYCLES),
        e.iadd32i(VALUE, VALUE, 1).sched(wait=1, delay=program.FIXED_LATENCY),
        guarded(stg(COUNTER, VALUE), ALIVE).sched(rd_bar=1, delay=1),
        membar_gpu().sched(delay=SLOW),
    ]


def code(form: str, log_stride: int, body_instructions: int) -> bytes:
    if form not in ('whole', 'tail', 'none', 'load_spin') or body_instructions not in (64, 256, 1024):
        raise ValueError('unknown interlock section form or body size')
    body = [e.ffma(24, 24, 25, 26).sched(delay=program.FIXED_LATENCY)] * body_instructions
    instrs = pixel_counter(log_stride) + alive()
    instrs += [e.mov32i(r, e.f32(v)).sched(delay=SLOW) for r, v in ((24, 0.0), (25, 1.0), (26, 1.0))]
    if form == 'none':
        instrs += body + count()
    else:
        instrs += leader_and_ticket()
        if form == 'tail':
            instrs += body
        spin = 'ldcg' if form == 'load_spin' else 'atom'
        instrs += wait(spin, len(instrs))
        if form != 'tail':
            instrs += body
        instrs += count()
        instrs += wait(spin, len(instrs))
        instrs.append(guarded(red_add(TICKET_WORD, INCREMENT), LEADER).sched(rd_bar=2, delay=e.BARRIER_SET_CYCLES))
    instrs += [program.SEPARATOR, *program.epilogue((VALUE, VALUE))]
    return e.bundle(e.finish(instrs))
