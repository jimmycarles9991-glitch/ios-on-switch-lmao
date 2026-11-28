import argparse
import csv
import struct
from collections import defaultdict
from collections.abc import Iterator
from pathlib import Path
from typing import NamedTuple

MAXWELL_B = 0xB197
DRAW_MACRO = 0x3A38
METHODS = {
    0x0210,
    0x0754,
    0x0DAC,
    0x0DB0,
    0x0F1C,
    0x0FA4,
    0x0FA8,
    0x0FB4,
    0x0FB8,
    0x0FBC,
    0x0FC0,
    0x0FC4,
    0x0FC8,
    0x0FD8,
    0x1130,
    0x1138,
    0x113C,
    0x1224,
    0x1260,
    0x15D0,
}


class Write(NamedTuple):
    submit: int
    channel: int
    application: int
    subchannel: int
    engine: int
    header_at: str
    header: int
    data_at: str
    method: int
    value: int


def submissions(directory: Path) -> Iterator[tuple[int, int, int, list[tuple[str, int]]]]:
    with (directory / 'submit.tsv').open() as f:
        manifest = csv.reader(f, delimiter='\t')
        for seq, row in enumerate(manifest):
            if row[0] != 'submit' or int(row[1]) != seq:
                raise ValueError(row)
            channel, _, count, application = map(int, row[2:])
            words = []
            for index in range(count):
                kind, number, entry, address, length, low, high = next(manifest)
                lo, hi = int(low, 16), int(high, 16)
                size = ((hi >> 10) & 0x1FFFFF) * 4
                va = ((hi & 255) << 32) | (lo & ~3)
                if (kind, int(number), int(entry), int(length), int(address, 16)) != ('entry', seq, index, size, va):
                    raise ValueError(f'inconsistent GPFIFO descriptor {seq}/{index}')
                if size:
                    data = (directory / f'push-{seq:04}-{index:03}.bin').read_bytes()
                    if len(data) != size:
                        raise ValueError(f'partial GPFIFO entry {seq}/{index}')
                    words.extend((f'{index}:{i}', x) for i, (x,) in enumerate(struct.iter_unpack('<I', data)))
            yield seq, channel, application, words


def decode(directory: Path) -> Iterator[Write]:
    classes = defaultdict(dict)
    for seq, channel, application, words in submissions(directory):
        # One packet can span several entries, including a separate entry containing only inline upload data.
        i = 0
        while i < len(words):
            at, word = words[i]
            i += 1
            if word == 0 or (word >> 16 in (1, 2, 3) and not word & 15):
                continue
            op = word >> 29
            reg = (word & 0xFFF) * 4
            sub = (word >> 13) & 7
            count = (word >> 16) & 0x1FFF
            if op in (0, 2) and not ((word >> 16) & 3):
                reg = word & 0x1FFC
                count = (word >> 18) & 0x7FF
                op = 1 if op == 0 else 3
            if op == 4:
                data = [(at, count)]
            elif op in (1, 3, 5) and i + count <= len(words):
                data = words[i : i + count]
                i += count
            else:
                raise ValueError(f'unsupported or truncated header {word:#010x} at {seq}/{at}')
            for j, (where, value) in enumerate(data):
                if reg == 0:
                    classes[channel][sub] = value & 0xFFFF
                yield Write(seq, channel, application, sub, classes[channel].get(sub, 0), at, word, where, reg, value)
                if op == 1 or (op == 5 and j == 0):
                    reg += 4


def draw_channel(directory: Path) -> tuple[list[Write], Write]:
    if 'vkQueueSubmit returned VK_SUCCESS' not in (directory / 'run.log').read_text().splitlines():
        raise ValueError(f'no successful application submission in {directory}')
    writes = [row for row in decode(directory) if row.engine == MAXWELL_B]
    draws = [i for i, row in enumerate(writes) if row.application and row.method == DRAW_MACRO and row.value == 3]
    if len(draws) != 1:
        raise ValueError(f'expected one R32 draw, found {len(draws)} in {directory}')
    draw = writes[draws[0]]
    end = draws[0] + 1
    while end < len(writes):
        row = writes[end]
        if (row.submit, row.channel, row.subchannel, row.method) != (
            draw.submit,
            draw.channel,
            draw.subchannel,
            DRAW_MACRO + 4,
        ):
            break
        end += 1
    return [row for row in writes[:end] if row.channel == draw.channel], draw


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('directory', type=Path)
    parser.add_argument(
        '--all', action='store_true', help='include other channels, classes and commands after the draw'
    )
    args = parser.parse_args()
    if args.all:
        rows = decode(args.directory)
    else:
        writes, draw = draw_channel(args.directory)
        rows = (row for row in writes if row.method in METHODS or row == draw)
    print(*Write._fields, sep='\t')
    for row in rows:
        print(
            row.submit,
            row.channel,
            row.application,
            row.subchannel,
            f'{row.engine:04x}',
            row.header_at,
            f'{row.header:08x}',
            row.data_at,
            f'{row.method:04x}',
            f'{row.value:08x}',
            sep='\t',
        )


if __name__ == '__main__':
    main()
