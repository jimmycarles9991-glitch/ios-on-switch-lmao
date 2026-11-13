#!/usr/bin/env python3
import argparse
import random
import struct
from pathlib import Path

from semantic_execution import MASK, TARGET, execute, program


def run(root, log):
    before = random.Random(256).randbytes(4096)
    expected = [(i * 73 + 19) ^ (i * i * 11) for i in range(256)]
    checks = 0
    for route in ('native', 'portable'):
        data = (root / f'semantic_table.{route}.data').read_bytes()
        assert data == struct.pack('<256I', *expected)
        code = program(log.read_text(), 'table', route)
        constants = {(1, 4 * i): word for i, word in enumerate(expected)}
        for offset in (0, 4, 8, 12, 16):
            for length in (0, 3, 4, 127, 1024, 4096):
                cb = constants | {(0, 0x720): offset, (2, 0): TARGET & MASK, (2, 4): TARGET >> 32, (2, 8): length}
                for index in range(512):
                    want = bytearray(before)
                    at = offset + index * 4
                    if at + 4 <= length:
                        struct.pack_into('<I', want, at, expected[index & 255])
                    got = execute(code, index, b'', before, 0, offset, 0, length, constant_words=cb)
                    assert got == want, (route, index, offset, length)
                    checks += 1
        cb[(1, 0)] ^= 1
        bad = execute(code, 0, b'', before, 0, offset, 0, length, constant_words=cb)
        assert bad != execute(code, 0, b'', before, 0, offset, 0, length, constant_words=cb | {(1, 0): expected[0]})
    print(f'PASS {checks} constant-table register executions and two data controls')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('output', type=Path)
    parser.add_argument('log', type=Path)
    args = parser.parse_args()
    run(args.output, args.log)
