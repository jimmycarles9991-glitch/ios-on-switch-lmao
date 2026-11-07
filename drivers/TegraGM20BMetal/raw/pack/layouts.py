#!/usr/bin/env python3
"""Per probe: set 0's layout as m2v + NVK wrote it, read off the tagged probe.

The probe is capture/probe.mm, read by its NVDEC_SCRUB records.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import probelog

STAGES = {1: 'v', 2: 'f'}


# TEXHEAD's TEXTURE_TYPE (word 4 bits 26:23): the probe binds 2D textures, so a 3D one is m2v's stand-in for a texture3d
# argument (Core Animation's colour cubes and LUTs)
TEXTURE_TYPE_3D = 2


def tic_kind(tic: tuple[str, list[int]], rts: dict[str, int]) -> tuple[str, int | str]:
    name, w = tic
    width, height = (w[4] & 0xFFFF) + 1, (w[5] & 0xFFFF) + 1
    if 16 <= width < 32 and height == width - 8:
        return ('tex', width - 16)
    if name in rts:
        return ('fetch', rts[name])
    if (w[4] >> 23) & 0xF == TEXTURE_TYPE_3D:
        return ('tex3d', name)
    return ('tic?', f'{name}:{width}x{height}')


DUMMY_TSC = [
    0x26092,
    0x291,
    0x40000,
]  # MTLSamplerDescriptor's defaults through m2v + NVK (nearest, clamp, not mipmapped)


def tsc_kind(t: list[int]) -> tuple[str, int | str]:
    # probe.mm tags fragment sampler slot s with max LOD 1 + s/2 under a nearest mip filter, linear filters when s&1 and
    # a repeating U when s&2. m2v hands Vulkan the level a nearest mip filter reads at the bound (metal/sampler.rs
    # nearest_level: past one half reads the next level), so the header holds level 1 + s//2 (4.8 fixed point: 256 per
    # level) and an odd slot shares its even neighbour's level: the filter tells them apart. (r10: reading the level
    # alone named an odd slot's sampler its even neighbour's, whose filter then differed, and 9 two-image Core Animation
    # states were skipped as "derived".)
    lod = (t[2] >> 12) & 0xFFF
    if lod % 256 or not 256 <= lod <= 256 * 8:
        return ('tsc?', ' '.join(f'{w:x}' for w in t))
    mag, u = t[1] & 7, t[0] & 7
    if mag not in (1, 2):
        return ('tsc?', ' '.join(f'{w:x}' for w in t))
    slot = 2 * (lod // 256 - 1) + (1 if mag == 2 else 0)
    if slot > 15 or u != (0 if slot & 2 else 2):
        return ('tsc?derived', ' '.join(f'{w:x}' for w in t))
    return ('smp', slot)


def layout(p: dict) -> tuple[list[tuple], list[str]]:
    r = p['records'][0]
    if 'set0' not in r:
        return [], []
    i = p['index']
    words = r['set0'][1]
    # render targets of the draw by name, from the canonical record (rtN=Rk ...)
    rts = {}
    for line in p['canon']:
        if line.startswith('  rt') and '=' in line:
            k, v = line.strip().split('=', 1)
            rts[v.split()[0]] = int(k[2:])
    entries, problems = [], []
    o = 0
    while o < len(words):
        key = f'ptr0.{o * 4}'
        if o % 4 == 0 and key in r['ptr']:
            pw = r['ptr'][key][1]
            if (pw[0] >> 16) == 0xC0DE and pw[1] == i:
                entries.append((o * 4, 'ubo', STAGES[(pw[0] >> 8) & 0xFF], pw[0] & 0xFF, words[o + 2]))
            else:
                problems.append(f'untagged pointer at {o * 4} ({pw[0]:x})')
            o += 4
            continue
        w = words[o]
        if w:
            tic, tsc = w & 0xFFFFF, w >> 20
            t = r['tic'].get(tic) if tic else None
            s = r['tsc'].get(tsc)
            tk = tic_kind(t, rts) if t else (None if not tic else ('tic?', tic))
            sk = tsc_kind(s) if s else ('tsc?', tsc)
            if sk[0] == 'tsc?' and tsc == 0 and s and s[:3] == DUMMY_TSC:
                sk = None  # index 0: the probe's warm-up sampler, never bound in a probe: the handle names no sampler
            entries.append((o * 4, 'handle', tk, sk, w))
            if tk and tk[0].endswith('?'):
                problems.append(f'unidentified handle at {o * 4}: {w:x}')
        o += 1
    return entries, problems


if __name__ == '__main__':
    P = probelog.load(sys.argv[1], sys.argv[2])
    import collections

    c = collections.Counter()
    for p in P:
        if p['draws'] != 1 or p['dispatches']:
            continue
        e, pr = layout(p)
        desc = ' '.join(
            f'{x[0]}:' + (f'ubo({x[2]}{x[3]},{x[4]:x})' if x[1] == 'ubo' else f'h({x[2]},{x[3]})') for x in e
        )
        c[desc] += 1
        if pr:
            print('PROBLEM', p['index'], pr)
    for k, n in c.most_common():
        print(n, k)
