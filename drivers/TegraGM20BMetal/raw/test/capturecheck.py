#!/usr/bin/env python3
"""Hold raw::Capture's records (runtime/raw_capture.h) to the pack's for the same states.

The capture decodes each probe submission again, primed from the pack's context init alone (the mock's
RAWCAP_CAPTURE, pack/cap.cpp), and records the registers a draw leaves other than that context. For every packed state
of the bucket: the same programs, the same push words, and each register of its record (the blob, and the baseline
every state shares) at the record's value, a register the capture leaves out at the context's (the probe log's
post-init registers).

usage: capturecheck.py <capture.txt> <probe256.out> <probe256.log> <pack.json> [barrier]
  barrier: the run is a bucket's barrier/ run (NVK's interlock hidden): its states' barrier-form records
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'pack'))
import json

import mkpack
import probelog


def captures(path: str) -> dict[int, list[dict]]:
    out: dict[int, list[dict]] = {}
    with open(path) as f:
        for line in f:
            fields = line.split()
            if fields[0] != 'CAPTURE':
                continue
            rec = {'vs': fields[3][3:], 'fs': fields[4][3:], 'push': {}, 'regs': {}}
            for kv in fields[6:]:
                k, v = kv.split('=')
                if k.startswith('p'):
                    rec['push'][int(k[1:], 16)] = int(v, 16)
                else:
                    rec['regs'][int(k, 16)] = int(v, 16)
            out.setdefault(int(fields[1]), []).append(rec)
    return out


def main() -> None:
    caps = captures(sys.argv[1])
    probes = probelog._probes(sys.argv[2])
    _, context = mkpack.load_init(sys.argv[3])
    with open(sys.argv[4]) as f:
        pack = json.load(f)
    form = sys.argv[5] if len(sys.argv) > 5 else ''
    pipelines = [r for r in pack['pipelines'] if r.get('form', '') == form]
    programs = [p['id'] for p in pack['programs']]
    baseline = {int(m, 16): v for m, v in pack['baseline'].items()}
    # the probes are the last submissions that draw (probelog.load); the capture names a submission by the mock's count
    drawing = sorted(caps)[-len(probes) :]
    by_index = {p['index']: caps[s][-1] for p, s in zip(probes, drawing, strict=True)}
    bad = 0
    for rec in pipelines:
        cap = by_index[rec['index']]
        why = []
        if (programs[rec['vs']], programs[rec['fs']]) != (cap['vs'], cap['fs']):
            why.append(f'programs {cap["vs"]} {cap["fs"]}')
        push = dict(rec['push'])
        if push != {o: v for o, v in cap['push'].items() if o in push} or set(cap['push']) - set(push):
            why.append('push words')
        for m, v in [*rec['blob'], *baseline.items()]:
            got = cap['regs'].get(m, context[0].get(m, 0))
            if got != v and m not in (0x2044, 0x2144):
                why.append(f'{m:x}={got:x} not {v:x}')
        bad += bool(why)
        if why:
            print(f'DIFFERENT {rec["index"]}: {"; ".join(why[:6])}')
    print(f'capturecheck: {len(pipelines) - bad} of {len(pipelines)} states read off the capture as packed')
    sys.exit(1 if bad or not pipelines else 0)


if __name__ == '__main__':
    main()
