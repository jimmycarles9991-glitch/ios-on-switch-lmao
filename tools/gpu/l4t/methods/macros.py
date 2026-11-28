import argparse
import csv
from itertools import groupby, pairwise
from pathlib import Path

from decode import Write, draw_channel


def programs(rows: list[Write]) -> dict[int, tuple[int, tuple[int, ...]]]:
    instructions = {}
    slots = {}
    pc, slot = 0, 0
    called = set()
    for row in rows:
        if row.method in (0x114, 0x118, 0x11C, 0x120) and called:
            raise ValueError('MME reprogrammed after first call')
        if row.method == 0x114:
            pc = row.value
        elif row.method == 0x118:
            instructions[pc] = row.value
            pc += 1
        elif row.method == 0x11C:
            slot = row.value
        elif row.method == 0x120:
            slots[slot] = row.value
            slot += 1
        elif row.method >= 0x3800 and row.method % 8 == 0:
            called.add((row.method - 0x3800) // 8)
    bounds = sorted({*slots.values(), max(instructions) + 1})
    ends = dict(pairwise(bounds))
    result = {}
    for index in called:
        start = slots[index]
        result[0x3800 + 8 * index] = (start, tuple(instructions[pc] for pc in range(start, ends[start])))
    return result


def list_code(directory: Path, code: dict[int, tuple[int, tuple[int, ...]]]) -> None:
    with (directory / 'macro-code.tsv').open('x') as f:
        out = csv.writer(f, delimiter='\t', lineterminator='\n')
        out.writerow(
            [
                'macro',
                'pc',
                'word',
                'op',
                'assignment',
                'exit_next',
                'dst',
                'src0',
                'src1',
                'immediate18',
                'address',
                'increment_bytes',
            ]
        )
        for method, (start, words) in sorted(code.items()):
            for pc, word in enumerate(words, start):
                op, assign = word & 15, (word >> 4) & 7
                x, y, dst = (word >> 11) & 7, (word >> 14) & 7, (word >> 8) & 7
                immediate = word >> 14
                address, step = '', ''
                if op != 7 and assign in (2, 5, 6, 7):
                    address = 'computed'
                    if op == 1 and x == 0:
                        address, step = f'{(immediate & 4095) * 4:04x}', str(((immediate >> 12) & 15) * 4)
                out.writerow(
                    [
                        f'{method:04x}',
                        pc,
                        f'{word:08x}',
                        op,
                        assign,
                        (word >> 7) & 1,
                        dst,
                        x,
                        y,
                        f'{immediate:05x}',
                        address,
                        step,
                    ]
                )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('capture', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    combined = {}
    with (args.output / 'macro-calls.tsv').open('x') as f:
        out = csv.writer(f, delimiter='\t', lineterminator='\n')
        out.writerow(['case', 'submit', 'header_at', 'method', 'arguments'])
        for case in sorted((args.capture / 'draws').iterdir()):
            if 'vkQueueSubmit returned VK_SUCCESS' not in (case / 'run.log').read_text().splitlines():
                continue
            rows, _ = draw_channel(case)
            for m, code in programs(rows).items():
                if combined.setdefault(m, code) != code:
                    raise ValueError(f'MME code differs for method {m:#x}')
            for _, packet in groupby(rows, lambda r: (r.submit, r.header_at)):
                writes = list(packet)
                first = writes[0]
                if first.method < 0x3800:
                    continue
                if first.method % 8 or any(r.method != first.method + 4 for r in writes[1:]):
                    raise ValueError(f'MME arguments span packets at {first}')
                out.writerow(
                    [
                        case.name,
                        first.submit,
                        first.header_at,
                        f'{first.method:04x}',
                        ' '.join(f'{r.value:x}' for r in writes),
                    ]
                )
    list_code(args.output, combined)


if __name__ == '__main__':
    main()
