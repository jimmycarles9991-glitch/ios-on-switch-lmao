#!/usr/bin/env python3
"""The codegen gate over two hot packs: each state's programs run on the same inputs, outputs compared.

usage: hotgate.py <before pack dir> <after pack dir> --corpus <hot corpus> [--states <hash prefix>...] [--sample N]
                  [--invocations N] [--seed S]

gate.py's comparison (interp.py's lanes over seeded attributes, constant buffers, texels and specials; a unorm
target's components compared as the codes GM20B's store makes) of every state the corpus names in both packs: its
default record's generic fragment program and each assumption variant, matched by state and position. --states
limits it to states whose key hash starts with one of the prefixes, --sample to N states chosen by the seed. A state
whose program uses an instruction the interpreter does not run counts as a failure, as a difference does. Vertex
programs are compared by their output attributes and stores. Each program runs twice: on the usual inputs, then on
inputs drawn mostly from gate.SPECIALS (zeros of both signs, infinities, NaNs, denormal floats and half pairs). A key
in one pack only is counted and fails the gate.
"""

import argparse
import random
import sys
from pathlib import Path

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from tools.gpu.maxwell import gate, hotstat


def programs(pack_dir: Path, corpus: Path) -> dict:
    import json  # noqa: PLC0415

    pack = json.loads((pack_dir / 'pack.json').read_text())
    heap = hotstat.Heap(pack_dir / 'pack.bin')
    found, _, _ = hotstat.programs_of(pack, corpus)
    out = {}
    for key, (_label, stage, index, state_hash) in found.items():
        if stage not in ('fs', 'vs') or '/' in key.rsplit(' @', 1)[1]:
            continue
        p = pack['programs'][index]
        out[key] = (heap.image(p), p.get('bound_textures', 0) - 1, f'{state_hash:016x}')
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('before', type=Path)
    ap.add_argument('after', type=Path)
    ap.add_argument('--corpus', type=Path, required=True)
    ap.add_argument('--states', nargs='*', default=[])
    ap.add_argument('--sample', type=int)
    ap.add_argument('--invocations', type=int, default=64)
    ap.add_argument('--seed', type=int, default=1)
    a = ap.parse_args()
    before, after = programs(a.before, a.corpus), programs(a.after, a.corpus)
    keys = sorted(k for k in before if k in after)
    unmatched = sorted(set(before) ^ set(after))
    for key in unmatched:
        print(f'only in {"before" if key in before else "after"}: {key[:160]}')
    if a.states:
        keys = [k for k in keys if any(before[k][2].startswith(s) for s in a.states)]
    if a.sample is not None:
        keys = sorted(random.Random(a.seed).sample(keys, min(a.sample, len(keys))))
    verdicts = {'identical': 0, 'different': 0, 'unsupported': 0}
    for key in keys:
        state_key = key.rsplit(' @', 1)[0]
        if key.rsplit(' @', 1)[1].startswith('vs'):
            verdict = gate.compare_vertex(before[key][0], after[key][0], a.invocations, a.seed)
            if verdict == 'identical':
                verdict = gate.compare_vertex(before[key][0], after[key][0], a.invocations, a.seed, gate.SPECIAL_HEAVY)
        else:
            verdict = gate.compare(before[key][:2], after[key][:2], a.invocations, a.seed, key=state_key)
            if verdict == 'identical':
                verdict = gate.compare(
                    before[key][:2], after[key][:2], a.invocations, a.seed, key=state_key, special=gate.SPECIAL_HEAVY
                )
        verdicts[verdict.split(':')[0]] += 1
        if verdict != 'identical':
            print(f'{before[key][2]} {key.rsplit(" @", 1)[1]}: {verdict}')
    print(
        f'hotgate: {len(keys)} programs ({sum(k.rsplit(" @", 1)[1].startswith("vs") for k in keys)} vertex): '
        + ', '.join(f'{v} {k}' for k, v in verdicts.items())
        + f'; {len(unmatched)} in one pack only'
    )
    sys.exit(1 if verdicts['different'] or verdicts['unsupported'] or unmatched else 0)


if __name__ == '__main__':
    main()
