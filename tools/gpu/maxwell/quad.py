"""A 2x2 quad of interp.py lanes, run in lockstep: cross-lane instructions (SHFL, FSWZADD, VOTE) and divergence.

Memory is modelled as interp.py's single lane has it: a load (SULD, LDG) is a pure function of its operands, a store
(SUST) is recorded and compared, an atomic returns 0, so the interlock's ticket wait passes at once.

Divergence follows Maxwell's reconvergence stack: SSY, PBK and PCNT push their targets; a branch some active lanes take
runs the rest first and parks the takers; SYNC (and an empty active set) resumes the next parked path, then reconverges
at the SSY target. The quad is the whole warp: VOTE and SHFL see four lanes.
"""

from tools.gpu.maxwell import isa
from tools.gpu.maxwell.execute import alu, branch_target
from tools.gpu.maxwell.floats import F32, arith
from tools.gpu.maxwell.interp import MAX_STEPS, Inputs, Lane, UnsupportedError
from tools.gpu.maxwell.isa import field as bits

LANES = 4
MEMORY_OPS = ('ATOMS', 'STG', 'BAR')
PUSHES = ('SSY', 'PBK', 'PCNT')


def fswzadd(op: int, a: int, b: int, mode: int, ftz: bool) -> int:
    if op == 3:
        return b
    x, y = ((a, b), (a, b ^ 0x80000000), (b, a ^ 0x80000000))[op]
    return arith('add', [x, y], F32, mode, ftz)


def cross_lane(name: str, w: int, lanes: list[Lane], active: set[int]) -> None:
    base = name.split('_', 1)[0]
    d = bits(w, 0, 8)
    before = [list(lane.regs) for lane in lanes]

    def reg(i: int, r: int) -> int:
        return 0 if r == 255 else before[i][r]

    for i in sorted(active):
        lane = lanes[i]
        if not lane.p(bits(w, 16, 3), bits(w, 19, 1)):
            continue
        if base == 'FSWZADD':
            a, b = reg(i, bits(w, 8, 8)), reg(i, bits(w, 20, 8))
            op = bits(w, 28 + 2 * i, 2)
            ftz = bool(bits(w, 44, 1))
            mode = bits(w, 39, 2)
            lane.w(d, fswzadd(op, a, b, mode, ftz))
        elif base == 'SHFL':
            b = bits(w, 20, 5) if bits(w, 28, 1) else reg(i, bits(w, 20, 8))
            op = bits(w, 30, 2)
            target = {0: b, 1: i - b, 2: i + b, 3: i ^ b}[op]
            inside = 0 <= target < LANES
            lane.w(d, reg(target if inside else i, bits(w, 8, 8)))
            lane.setp(bits(w, 48, 3), inside)
        else:
            votes = [lanes[j].p(bits(w, 39, 3), bits(w, 42, 1)) for j in sorted(active)]
            mode = bits(w, 48, 2)
            lane.w(d, sum(1 << j for j in sorted(active) if lanes[j].p(bits(w, 39, 3), bits(w, 42, 1))))
            lane.setp(bits(w, 45, 3), (all(votes), any(votes), all(votes) or not any(votes))[mode])


def run_quad(code: bytes, inputs: list[Inputs], outputs: int) -> list[tuple]:  # noqa: C901, PLR0912, PLR0915  every control opcode here
    """Run a quad to EXIT; give each lane's first `outputs` registers, whether it was killed, and its stores."""
    words = isa.words_of(code)
    lanes = [Lane() for _ in range(LANES)]
    results: list[tuple | None] = [None] * LANES
    done: set[int] = set()
    pc, active, stack = 8, set(range(LANES)), []
    steps = 0

    def resume() -> tuple[int, set[int]] | None:
        while stack:
            _, target, mask = stack.pop()
            mask = mask - done
            if mask:
                return target, mask
        return None

    while True:
        steps += 1
        if steps > MAX_STEPS:
            raise UnsupportedError('no EXIT')
        if (pc // 8) % 4 == 0:
            pc += 8
            continue
        w = words[pc // 8]
        name = isa.name(w)
        base = name.split('_', 1)[0]
        if base in MEMORY_OPS or name == 'UNKNOWN':
            raise UnsupportedError(name)
        guard = {i for i in active if lanes[i].p(bits(w, 16, 3), bits(w, 19, 1))}
        nxt = pc + 8
        if name in PUSHES:
            stack.append((name, branch_target(pc, w), set(active)))
        elif name == 'BRA':
            if guard == active:
                nxt = branch_target(pc, w)
            elif guard:
                stack.append(('DIV', branch_target(pc, w), guard))
                active = active - guard
        elif name in ('SYNC', 'BRK', 'CONT', 'EXIT', 'KIL'):
            if name in ('EXIT', 'KIL'):
                for i in guard:
                    lanes[i].killed = name == 'KIL'
                    results[i] = ([lanes[i].r(r) for r in range(outputs)], lanes[i].killed, lanes[i].stores)
                done |= guard
            elif name in ('BRK', 'CONT') and guard and guard != active:
                raise UnsupportedError(f'divergent {name}')
            leaving = guard if name != 'SYNC' else active
            if name in ('BRK', 'CONT') and guard:
                want = 'PBK' if name == 'BRK' else 'PCNT'
                while stack and stack[-1][0] != want:
                    stack.pop()
                if not stack:
                    raise UnsupportedError(f'{name} outside its loop')
                _, target, mask = stack.pop()
                nxt, active = target, mask - done
            elif leaving == active or name == 'SYNC':
                found = resume()
                if found is None:
                    break
                nxt, active = found
            else:
                active = active - leaving
        elif base in ('SHFL', 'FSWZADD', 'VOTE'):
            cross_lane(name, w, lanes, active)
        elif name != 'NOP':
            for i in guard:
                alu(name, w, lanes[i], inputs[i])
        pc = nxt
    return [
        r if r is not None else ([lanes[i].r(k) for k in range(outputs)], False, lanes[i].stores)
        for i, r in enumerate(results)
    ]
