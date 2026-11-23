#!/usr/bin/env python3
"""mkformats (r8-rawca): NVK's texture header for each format of a PROBE_FORMATS run (probe.mm).

As the driver writes it for a pitch-linear 2D texture: prints "<MTLPixelFormat> <8 header words with the address bits
cleared>" per format NVK took.
usage: mkformats.py <probe.out> <probe.log>
"""

import sys

with open(sys.argv[1]) as probe_out:
    out = [line.split() for line in probe_out if line.startswith('FORMAT ') and 'refused' not in line]
subs, cur = [], []
with open(sys.argv[2]) as probe_log:
    for line in probe_log:
        if line.startswith('SUBMIT'):
            subs.append(cur)
            cur = []
        else:
            cur.append(line.rstrip('\n'))
# the decoded submissions with a draw, in order: one per FORMAT line
draws = [s for s in subs if any(x.startswith('DRAW') for x in s)]
assert len(draws) >= len(out), (len(draws), len(out))
draws = draws[-len(out) :]
for f, s in zip(out, draws, strict=False):
    _, i, mtl, size, bpr = f
    w, h = map(int, size.split('x'))
    tics = [x.split() for x in s if x.startswith('P tic ')]
    hit = [t for t in tics if (int(t[8], 16) & 0xFFFF) + 1 == w and (int(t[9], 16) & 0xFFFF) + 1 == h]
    if not hit:
        print(mtl, 'none')
        continue
    words = [int(x, 16) for x in hit[0][4:12]]
    words[1] &= 0x1F
    words[2] &= ~0xFFFF  # address bits (pitch: ADDRESS_BITS31TO5 in word 1, 47:32 in word 2)
    print(mtl, bpr, ' '.join(f'{x:08x}' for x in words))
