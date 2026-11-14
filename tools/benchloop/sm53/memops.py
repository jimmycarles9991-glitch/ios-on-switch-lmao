"""Special registers, global memory, votes and carries, as NAK's sm50.rs encodes them (its interlock's words agree).

Global addresses are 64-bit register pairs (.E), the low word in an even register.
"""

from dataclasses import replace

from tools.benchloop.sm53.encode import Instr, Pred, _Word

SR_LANE_ID = 0x00
SR_VIRTID = 0x03
SR_ORDERING_TICKET = 0x0F
SR_THREAD_KILL = 0x13
SR_AFFINITY = 0x1C
LD_CACHE = {'ca': 0, 'cg': 1, 'ci': 2, 'cv': 3}
B32 = 4
MEMBAR_GPU = 1


def s2r(d: int, sr: int) -> Instr:
    return _Word('S2R').reg(0, d).put(20, 8, sr).instr()


def _pair(addr: int) -> int:
    if addr % 2:
        raise ValueError(f'R{addr}: a 64-bit address is an even register pair')
    return addr


def atom_add(d: int, addr: int, data: int) -> Instr:
    return _Word('ATOM').reg(0, d).reg(8, _pair(addr)).reg(20, data).flag(48, True).instr()


def red_add(addr: int, data: int) -> Instr:
    return _Word('RED').reg(0, data).reg(8, _pair(addr)).flag(48, True).instr()


def ldg(d: int, addr: int, cache: str = 'cg') -> Instr:
    """Give LD.E.<cache>.32 d, [addr]; 'cg' reads L2, past the SM's L1."""
    w = _Word('LDG').reg(0, d).reg(8, _pair(addr)).flag(45, True).put(46, 2, LD_CACHE[cache])
    return w.put(48, 3, B32).instr()


def stg(addr: int, data: int) -> Instr:
    return _Word('STG').reg(0, data).reg(8, _pair(addr)).flag(45, True).put(48, 3, B32).instr()


def membar_gpu() -> Instr:
    return _Word('MEMBAR').put(8, 2, MEMBAR_GPU).instr()


def vote_any(d: int, vote: int, src: int | Pred) -> Instr:
    """Give VOTE.ANY: the warp's ballot of `src` in d, whether any lane's is true in predicate `vote`."""
    return _Word('VOTE').reg(0, d).put(45, 3, vote).pred(39, src).put(48, 2, 1).instr()


def guarded(i: Instr, pred: int, neg: bool = False) -> Instr:
    return replace(i, word=i.word & ~(0xF << 16) | (pred | int(neg) << 3) << 16)


def carry_out(i: Instr) -> Instr:
    return replace(i, word=i.word | 1 << 47)


def carry_in(i: Instr) -> Instr:
    return replace(i, word=i.word | 1 << 43)
