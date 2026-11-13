#!/usr/bin/env python3
import itertools
import json
import math
import struct
import sys
from pathlib import Path

from semantic_execution import MASK, TARGET, execute, program

SHAPES = [(8, 5, 3), (3, 1, 1), (8, 1, 3), (3, 5, 1), (3, 5, 3), (8, 5, 1), (3, 1, 3), (8, 1, 1)]
NOMINAL, GRID = (8, 5, 3), (19, 11, 4)
BASES = [tuple(0 if s == n else g // n for s, n, g in zip(size, NOMINAL, GRID)) for size in SHAPES]
REGIONS = [tuple(g // n if s == n else 1 for s, n, g in zip(size, NOMINAL, GRID)) for size in SHAPES]


def expected(name, index, local, group, size, base, nominal, grid, before):
    group = tuple(a + b for a, b in zip(group, base))
    global_id = tuple(a * b + c for a, b, c in zip(group, nominal, local))
    out = bytearray(before)
    if name == 'edges_barrier':
        words = [global_id[0] ^ (global_id[1] << 8) ^ (global_id[2] << 16), *size]
    else:
        words = [
            *global_id,
            0x101,
            *group,
            0x202,
            *grid,
            0x303,
            *size,
            0x404,
            *local,
            0x505,
            *(math.ceil(a / b) for a, b in zip(grid, nominal)),
            0x606,
            index,
            index % 32,
            index // 32,
            32,
        ]
    struct.pack_into('<' + 'I' * len(words), out, index * 128, *words)
    return out


def constants(meta, groups, base, nominal, grid, length):
    words = {(0, 0): 0, (0, 4): 0, (0, 8): 0, (1, 0): TARGET & MASK, (1, 4): TARGET >> 32, (1, 8): length}
    words.update({(0, 12 + 4 * i): value for i, value in enumerate(groups)})
    words.update({(0, 0x700 + 4 * i): word for i, word in enumerate(meta['push'])})
    region = [*base, sum((n - 1) << (10 * i) for i, n in enumerate(nominal)), *grid, 0]
    words.update({(0, 0x700 + 4 * (meta['regionWord'] + i)): word for i, word in enumerate(region)})
    return words


def run(root):
    text = (root / 'compiled.log').read_text()
    checks = negative = 0
    for name in ('edges', 'edges_barrier'):
        for shape, size in enumerate(SHAPES):
            for mode in (0, 1):
                meta = json.loads((root / f'semantic_{name}_{shape}_{mode}.json').read_text())
                assert meta['regionWord'] >= 0
                groups = REGIONS[shape] if mode else (3, 5, 2)
                base = BASES[shape] if mode else (0, 0, 0)
                nominal = NOMINAL if mode else size
                grid = GRID if mode else tuple(a * b for a, b in zip(groups, size))
                workgroups = list(itertools.product(*(range(n) for n in groups)))
                if not mode:
                    workgroups = [workgroups[i] for i in (0, len(workgroups) // 2, -1)]
                before = bytes([0xA5]) * (128 * math.prod(size))
                cb = constants(meta, groups, base, nominal, grid, len(before))
                for route in ('native', 'portable'):
                    code = program(text, f'{name}_{shape}_{mode}', route)
                    if name == 'edges_barrier':
                        operations = [line for block in code[0].values() for line in block[0]]
                        assert any('bar.sync' in line for line in operations) == (math.prod(size) > 32)
                        assert any('membar.sc.cta' in line for line in operations)
                    for group in workgroups:
                        for local in itertools.product(*(range(n) for n in size)):
                            index = (local[2] * size[1] + local[1]) * size[0] + local[0]
                            sr = {f'sr[0x{0x21 + i:x}]': value for i, value in enumerate(local)}
                            sr.update({f'sr[0x{0x25 + i:x}]': value for i, value in enumerate(group)})
                            want = expected(name, index, local, group, size, base, nominal, grid, before)
                            got = execute(
                                code, index, b'', before, 0, 0, 0, len(before), constant_words=cb, system_values=sr
                            )
                            assert got == want, (
                                name,
                                shape,
                                mode,
                                route,
                                local,
                                group,
                                struct.unpack_from('<28I', got, index * 128),
                                struct.unpack_from('<28I', want, index * 128),
                            )
                            checks += 1
                    if mode and shape == 1:
                        local = group = (0, 0, 0)
                        sr = {f'sr[0x{i:x}]': 0 for i in (0x21, 0x22, 0x23, 0x25, 0x26, 0x27)}
                        want = expected(name, 0, local, group, size, base, nominal, grid, before)
                        control = program(text, f'{name}_{shape}_0', route)
                        got = execute(
                            control, 0, b'', before, 0, 0, 0, len(before), constant_words=cb, system_values=sr
                        )
                        assert got != want, 'single-region negative control failed to distinguish edge semantics'
                        negative += 1
                        for field in (0, 3, 4) if name == 'edges' else (0, 3):
                            mutated = dict(cb)
                            key = (0, 0x700 + 4 * (meta['regionWord'] + field))
                            mutated[key] ^= 1
                            got = execute(
                                code, 0, b'', before, 0, 0, 0, len(before), constant_words=mutated, system_values=sr
                            )
                            assert got != want, ('region mutation was not observed', name, field)
                            negative += 1
    print(f'PASS {checks} compiled register-IR builtin comparisons and {negative} negative controls')
    print('Reciprocal instructions use an RNE model; actual MUFU accuracy remains a hardware check.')
    print("Barrier checks use each invocation's own shared slot; cross-invocation visibility remains a hardware check.")


if __name__ == '__main__':
    run(Path(sys.argv[1]))
