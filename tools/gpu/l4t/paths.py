"""Control-flow estimate for a coherent quad with first-pass loop exits."""

from functools import cache

from tools.gpu.maxwell import cost, isa
from tools.gpu.maxwell.execute import branch_target

PUSH = {'SSY', 'PBK', 'PCNT'}
POP = {'SYNC': 'SSY', 'BRK': 'PBK', 'CONT': 'PCNT'}
TERMINATE = {'EXIT', 'KIL'}
UNSUPPORTED = {'BRX', 'JMP', 'JMX', 'CAL', 'JCAL', 'RET', 'PRET', 'LONGJMP', 'BPT', 'PLONGJMP'}


CONTROL = PUSH | POP.keys() | TERMINATE | {'BRA'}
Stack = tuple[tuple[str, int], ...]
Edge = tuple[int, Stack]
Endpoints = tuple[int, int, float, float]


def successors(index: int, stack: Stack, op: str, word: int) -> list[Edge | None]:
    target = branch_target((index - 1) * 8, word) // 8
    if op in PUSH:
        return [(index, (*stack, (op, target)))]
    pred = word >> 16 & 15
    unconditional = pred == 7 and word & 31 == 15
    edges = [] if unconditional else [(index, stack)]
    if pred == 15:
        return edges
    if op == 'BRA':
        edges.append((target, stack))
    elif op in POP:
        position = next((i for i in reversed(range(len(stack))) if stack[i][0] == POP[op]), None)
        if position is None:
            raise ValueError(f'unmatched {op} at {(index - 1) * 8:#x}')
        edges.append((stack[position][1], stack[:position]))
    else:
        edges.append(None)
    return edges


def basic_block(words: list[int], names: list[str], index: int) -> tuple[int, int, float]:
    slots = 0
    cycles = 0.0
    while index < len(words):
        word, op = words[index], names[index]
        index += 1
        if op == 'SCHED':
            continue
        if op in UNSUPPORTED:
            raise ValueError(f'unsupported control flow at {(index - 1) * 8:#x}: {op}')
        slots += 1
        cycles += (cost.issue_cycles(op) or 1) + cost.bank_extra(cost.gpr_sources(word, op), distinct=True)
        if op in CONTROL:
            return index, slots, cycles
    raise ValueError('control flow runs past code')


def coherent_paths(code: bytes) -> dict:
    words = isa.words_of(code)
    names = [isa.name(w) if i % 4 else 'SCHED' for i, w in enumerate(words)]
    active = set()
    back_edges = set()

    @cache
    def walk(index: int, stack: Stack) -> Endpoints | None:
        start = index, stack
        active.add(start)
        index, slots, cycles = basic_block(words, names, index)
        edges = successors(index, stack, names[index - 1], words[index - 1])
        results = []
        for edge in edges:
            if edge is None:
                results.append((0, 0, 0, 0))
            elif edge in active:
                back_edges.add((index - 1, edge[0]))
            else:
                endpoints = walk(*edge)
                if endpoints is not None:
                    results.append(endpoints)
        active.remove(start)
        if results:
            return (
                slots + min(x[0] for x in results),
                slots + max(x[1] for x in results),
                cycles + min(x[2] for x in results),
                cycles + max(x[3] for x in results),
            )
        return None

    result = walk(0, ())
    walk.cache_clear()
    if result is None:
        raise ValueError('no terminating path')
    return {'slots': result[:2], 'cycles': result[2:], 'cut_edges': len(back_edges)}
