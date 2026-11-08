#!/usr/bin/env python3
"""targetcheck: the raw path's render target formats (runtime/raw_gpu.cpp kFormats) against NVK's.

NVK's are the color target format of each draw of a PROBE_TARGETS run (pack/probe.mm): one draw per Metal format, its
first attachment of that format, its second the state's own. Every format NVK renders must have NVK's target format in
the table, and no other.
usage: targetcheck.py <probe.out> <probe.log> <raw_gpu.cpp>
"""

import re
import sys


def nvk_targets(out_path: str, log_path: str) -> dict[int, int]:
    with open(out_path) as out:
        drawn = [int(line.split()[2]) for line in out if line.startswith('TARGET ') and 'refused' not in line]
    # the state's draws (two attachments); m2v's own draws (a new float texture's fresh pattern) have one
    draws = []
    with open(log_path) as log:
        for line in log:
            if line.startswith('DRAW'):
                draws.append({})
            m = re.match(r'\s+rt(\d)=\S+ \d+x\d+ fmt=([0-9a-f]+)', line)
            if m and draws:
                draws[-1][int(m[1])] = int(m[2], 16)
    formats = [d[0] for d in draws if 0 in d and 1 in d]
    if len(formats) < len(drawn):
        sys.exit(f'{len(drawn)} targets drawn, {len(formats)} draws decoded')
    return dict(zip(drawn, formats[-len(drawn) :], strict=True))


def raw_targets(source: str) -> dict[int, int]:
    with open(source) as f:
        text = f.read()
    table = text[
        text.index('const FormatInfo kFormats[]') : text.index('};', text.index('const FormatInfo kFormats[]'))
    ]
    return {int(m[1]): int(m[2], 0) for m in re.finditer(r'\{(\d+), (0x[0-9a-f]+|0), ', table)}


def main() -> int:
    nvk = nvk_targets(sys.argv[1], sys.argv[2])
    raw = raw_targets(sys.argv[3])
    bad = 0
    for mtl, rt in sorted(raw.items()):
        want = nvk.get(mtl)
        if want is None:
            if rt:
                print(f'format {mtl}: raw renders it as {rt:#x}, NVK did not draw it')
                bad += 1
        elif rt != want:
            print(f'format {mtl}: raw {rt:#x}, NVK {want:#x}')
            bad += 1
    print(f'{len(raw)} formats, {len(nvk)} drawn by NVK, {bad} differ')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
