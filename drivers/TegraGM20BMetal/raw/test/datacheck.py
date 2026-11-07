#!/usr/bin/env python3
"""Hold datacheck's raw draws against the probe's NVK twin draws of the same states.

For each state, per stage: the same program (nvdec names it by its header, code and shader data), its shader data bound
at the same constant buffer slot with the same size and bytes, and set 0 at the same slot with the same size. Set 0's
bytes differ by design (the raw path's textures are its own), so they are not compared.

usage: datacheck.py <datacheck stdout> <datacheck RAWCAP_LOG> <bucket dir (rawpack.py --probe-bucket)>...
"""

import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'pack'))
import packfile
import probelog

CBUF = re.compile(r'cb(\d+) type=(\d+) size=([0-9a-f]+) data=(\S+)')
SHADER_DATA, SET = '2', '3'


def key_hash(key: str) -> int:
    own = re.search(r'\| f (ycbcr_\w+)', key)
    return packfile.fnv(('raw.ycbcr.' + own.group(1)).encode() if own else key.encode())


def bindings(lines: list[str]) -> dict[str, list[tuple]]:
    """Give each stage's program and the constant buffers of type shader data (whole) and set 0 (slot and size)."""
    out, stage = {}, None
    for line in (s.strip() for s in lines):
        if line.startswith('stage'):
            stage = line.split()[0]
            out[stage] = [line.split()[1]]
        elif stage and (m := CBUF.match(line)):
            slot, kind, size, data = m.groups()
            if kind == SHADER_DATA:
                out[stage].append((slot, kind, size, data))
            elif kind == SET:
                out[stage].append((slot, kind, size))
    return out


def raw_draws(log_path: str) -> list[list[str]]:
    draws, cur = [], []
    with open(log_path) as log:
        for text in log:
            line = text.rstrip('\n')
            if line.startswith('SUBMIT'):
                if any(x.startswith('DRAW') for x in cur):
                    draws.append(cur)
                cur = []
            else:
                cur.append(line)
    return draws


def main() -> None:
    with open(sys.argv[1]) as out:
        keys = [int(line.split()[1], 16) for line in out if line.startswith('DATACHECK ')]
    twin = {}
    for d in sys.argv[3:]:
        for p in probelog.load(os.path.join(d, 'probe256.out'), os.path.join(d, 'probe256.log')):
            twin.setdefault(key_hash(p['key']), p['canon'])
    draws = raw_draws(sys.argv[2])
    if len(draws) != len(keys):
        sys.exit(f'datacheck: {len(keys)} states drawn, {len(draws)} raw submissions with a draw')
    bad = 0
    for h, raw in zip(keys, draws, strict=True):
        want, got = bindings(twin.get(h, [])), bindings(raw)
        same = bool(want) and want == got
        bad += not same
        print(f'{"same" if same else "DIFFERENT"} {h:016x}: twin {want} raw {got}')
    print(f'datacheck: {len(keys) - bad} of {len(keys)} states bind as the twin does')
    sys.exit(1 if bad or not keys else 0)


if __name__ == '__main__':
    main()
