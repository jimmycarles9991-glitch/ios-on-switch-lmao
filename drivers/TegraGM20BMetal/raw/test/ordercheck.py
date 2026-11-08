#!/usr/bin/env python3
"""ordercheck (r8-rawca): reads the mock GPU's log (mock_iokit.c MOCK_GPU_LOG) of an orderframe run.

The raw channel is the last one made (the facade opens after m2v), the rest are NVK's; with <first raw channel> (a run
whose raw channel was killed and made again: mock_iokit.c MOCK_KILL), every channel from it is the raw path's. Every
submission must start after every submission of the other driver kicked before it ended (the commit order, kept on the
GPU), and the raw submit thread must have made no WaitFence call.
usage: ordercheck.py <log> <begin_us> [first raw channel]
"""

import sys

runs, stats = [], {}
with open(sys.argv[1]) as log:
    for line in log:
        w = line.split()
        if w[0] == 'run':
            runs.append(
                {
                    'ch': int(w[1]),
                    'seq': int(w[2]),
                    'kick': int(w[3]),
                    'start': int(w[4]),
                    'end': int(w[5]),
                    'acq': int(w[6]),
                }
            )
        elif w[0] == 'order':
            stats['order'] = (int(w[2]), int(w[4]))
        elif w[0] == 'waitfence':
            stats['wait'] = (int(w[2]), int(w[4]))
begin = int(float(sys.argv[2]) * 1000)
rawch = int(sys.argv[3]) if len(sys.argv) > 3 else max(r['ch'] for r in runs)
# a raw channel made again starts with its context (raw_gpu.cpp Device::reopen), its own state that nothing else reads
window = sorted(
    (r for r in runs if r['kick'] >= begin and not (r['ch'] > rawch and r['seq'] == 1)), key=lambda r: r['kick']
)
bad = 0
for i, r in enumerate(window):
    for p in window[:i]:
        if (p['ch'] >= rawch) != (r['ch'] >= rawch) and r['start'] < p['end']:
            bad += 1
            print(
                f'ORDER BROKEN: ch {r["ch"]} #{r["seq"]} started at {r["start"]} before ch {p["ch"]} #{p["seq"]} '
                f'(kicked earlier) ended at {p["end"]}'
            )
raw = [r for r in window if r['ch'] >= rawch]
nvk = [r for r in window if r['ch'] < rawch]
channels = sorted({r['ch'] for r in nvk})
acquires = sum(1 for r in window if r['acq'])
kicks = (window[-1]['kick'] - window[0]['kick']) / 1000
busy = (window[-1]['end'] - window[0]['start']) / 1000
print(
    f'runs in window: raw {len(raw)} (channels {sorted({r["ch"] for r in raw})}), nvk {len(nvk)} '
    f'(channels {channels}); with acquires: '
    f'{acquires}; kicks within {kicks:.1f} ms, GPU busy {busy:.1f} ms'
)
print(
    f'order calls {stats["order"][0]}, acquires queued {stats["order"][1]}; WaitFence calls {stats["wait"][0]}, '
    f'from the raw submit thread {stats["wait"][1]}'
)
print('ordering', 'OK' if not bad else f'BROKEN ({bad})')
sys.exit(1 if bad else 0)
