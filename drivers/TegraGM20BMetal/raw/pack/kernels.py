"""The compute kernels of the probe's compute runs (probecompute.mm), for mkpack.py.

A bucket's compute.out names each probed dispatch shape (KPROBE <i> tg=... grid=... key=<key>), the grid's width its
own, the shape's second dispatch the width after it (the tags bound from elsewhere) and its third the one after that
(each buffer bound residual_code bytes past its tag, which names its residual word). compute.log holds nvdec's
record of each dispatch (nvdec_compute.cpp, NVDEC_KERNELS): its QMD and program (K qmd=), the root table's push words
(K push), m2v's DispatchRegion field (K region), set 0's size (K set) and what each of set 0's words holds (K entry).
Tags (probecompute.mm): a buffer slot's bytes start 0xC0DE0000 | slot, texture slot t is (16 + t) x (8 + t). Every
sampler's header goes in the pack's samplers: the key holds the samplers bound, so the header is the record's own.
"""

import collections
import os
import re

KPROBE = re.compile(r'KPROBE (\d+) tg=(\d+),(\d+),(\d+) grid=(\d+),\S+ key=(.*)$')
TAG_BYTES = 4096
TEXTURE_TAG = (16, 8)
CONSTEXPR = 0x80  # PackKernelEntry.slot of a constexpr sampler: its index in the pack's constexpr samplers
BUFFER, TEXTURE, SAMPLER = 0, 1, 2  # runtime/raw_pack.h PackKernelEntryKind


def residual_code(slot: int) -> int:
    """Give a buffer slot's residual in the bind-offset dispatch (pack/probe_bind.h probeComputeCode)."""
    return 1 + slot % 15


def attach_residuals(k: dict, offset: dict | None) -> None:
    """Give each of `k`'s buffer entries ('residual') the root push word its SlotBindOffset residual goes to, else None.

    `offset` is the bind-offset dispatch's kernel: the words it set where the plain dispatch left 0, by their value's
    low byte (runtime/raw_residual.h residualWordsByValue), name each slot by its code; a code two of the kernel's
    buffer slots share, or one two words hold, names no word. A kernel whose program, QMD or set-0 layout moved with the
    offsets takes none (its buffers stay on 16-byte offsets), as does one whose bind-offset dispatch nvdec did not
    decode (`offset` None).
    """
    buffers = [e for e in k['entries'] if e['kind'] == BUFFER]
    for e in buffers:
        e['residual'] = None
    if offset is None:
        return
    layout = [(e['off'], e['kind'], e['slot']) for e in k['entries']]
    moved = [(e['off'], e['kind'], e['slot']) for e in offset['entries']]
    if any(k.get(f) != offset.get(f) for f in ('prog', 'qmd', 'region', 'set_bytes')) or layout != moved:
        return
    changed = [(v & 0xFF, w) for w, v in offset['push'].items() if v and not k['push'].get(w)]
    held = collections.Counter(v for v, _ in changed)
    words = {v: w for v, w in changed if held[v] == 1}
    codes = collections.Counter(residual_code(e['slot']) for e in buffers)
    for e in buffers:
        code = residual_code(e['slot'])
        if codes[code] == 1:
            e['residual'] = words.get(code)


def blocks(log: str) -> list[list[str]]:
    """Give each dispatch's K lines, in order."""
    out: list[list[str]] = []
    with open(log) as f:
        for line in f:
            if line.startswith('K qmd='):
                out.append([])
            if line.startswith('K ') and out:
                out[-1].append(line.rstrip('\n'))
    return out


def template(qmd: list[int]) -> list[int]:
    """Give the QMD with what each dispatch sets cleared: its program offset, grid and constant buffer addresses."""
    q = list(qmd)
    q[8] = q[12] = q[13] = 0
    for i in range(8):
        q[29 + 2 * i] = 0
        q[30 + 2 * i] &= ~0xFF
    return q


def sampler_slot(words: list[int], constexpr_tsc: list[list[int]], problems: list[str]) -> int:
    """Give a sampler handle's entry slot: its header in the pack's samplers (the key holds the samplers bound)."""
    if words not in constexpr_tsc:
        constexpr_tsc.append(words)
    if constexpr_tsc.index(words) >= CONSTEXPR:
        problems.append('a sampler past the 128 an entry names')
    return CONSTEXPR | (constexpr_tsc.index(words) & (CONSTEXPR - 1))


def width(lines: list[str]) -> int:
    """Give the dispatch's grid width (QMD CTA_RASTER_WIDTH), which names its shape."""
    return int(lines[0].split()[1][4:].split(',')[12], 16)


def entry(f: list[str], constexpr_tsc: list[list[int]], problems: list[str]) -> dict:
    """Give a K entry line's set-0 entry (its fields `f`)."""
    if f[3] == 'buffer':
        window = int(f[5], 16)
        return {'off': int(f[2]), 'kind': BUFFER, 'slot': int(f[4]), 'window': window if window < TAG_BYTES else 0}
    if f[3] == 'texture':
        w, h = int(f[4]), int(f[5])
        if w < TEXTURE_TAG[0] or h != w - TEXTURE_TAG[0] + TEXTURE_TAG[1]:
            problems.append(f'a texture of no tag ({w}x{h}) at {f[2]}')
        # the header's word 0 (format and swizzle): m2v may bind a view of another format (a half texture's raw bits),
        # which the raw path, binding the texture's own header, refuses (raw_compute.cpp)
        return {'off': int(f[2]), 'kind': TEXTURE, 'slot': w - TEXTURE_TAG[0], 'window': int(f[6], 16)}
    slot = sampler_slot([int(x, 16) for x in f[4:12]], constexpr_tsc, problems)
    return {'off': int(f[2]), 'kind': SAMPLER, 'slot': slot, 'window': 0}


def kernel(probe: re.Match[str], lines: list[str], constexpr_tsc: list[list[int]]) -> tuple[dict, list[str]]:
    k = {'key': probe.group(6), 'region': -1, 'set_bytes': 0, 'push': {}, 'entries': []}
    problems, push = [], {}
    for line in lines:
        f = line.split()
        if f[1].startswith('qmd='):
            k['qmd'] = template([int(x, 16) for x in f[1][4:].split(',')])
            k['prog'] = f[2].removeprefix('prog=')
        elif f[1] == 'push':
            push = {int(w): int(v, 16) for w, v in (x.split('=') for x in f[2:])}
        elif f[1] == 'region':
            k['region'] = int(f[2])
        elif f[1] == 'set':
            k['set_bytes'] = int(f[2], 16)
        elif f[1] == 'entry':
            k['entries'].append(entry(f, constexpr_tsc, problems))
    region = range(k['region'], k['region'] + 8) if k['region'] >= 0 else range(0)
    k['push'] = {w: v for w, v in push.items() if w not in region}
    if len(k['entries']) > 16:
        problems.append(f'{len(k["entries"])} set-0 entries')
    if 'qmd' not in k:
        problems.append('no QMD')
    return k, problems


def load(dirs: list[str], constexpr_tsc: list[list[int]]) -> tuple[list[dict], list[tuple[str, str]]]:
    """Give the kernels of every bucket's compute run, each key once, and the shapes left out with why."""
    kernels, skipped, seen = [], [], set()
    for d in dirs:
        out, log = os.path.join(d, 'compute.out'), os.path.join(d, 'compute.log')
        if not (os.path.exists(out) and os.path.exists(log)):
            continue
        with open(out) as f:
            probes = {int(m.group(5)): m for m in map(KPROBE.match, f) if m}
        dispatched = {width(lines): lines for lines in blocks(log)}
        for grid, probe in probes.items():
            lines, moved, off = dispatched.get(grid), dispatched.get(grid + 1), dispatched.get(grid + 2)
            if not lines or not moved:
                skipped.append((probe.group(6), 'no dispatch decoded'))
                continue
            if probe.group(6) in seen:
                continue
            # a left-out kernel's samplers stay out of the pack's
            samplers = list(constexpr_tsc)
            k, problems = kernel(probe, lines, samplers)
            other, _ = kernel(probe, moved, list(samplers))
            bound = kernel(probe, off, list(samplers))[0] if off else None
            # the second dispatch bound the same tags from other addresses, through other samplers: a program, QMD or
            # push word that moved with them is a value the bindings decide, which the raw path does not compute
            if any(k.get(f) != other.get(f) for f in ('prog', 'qmd', 'push', 'region', 'set_bytes', 'entries')):
                problems.append('a program or push word the bound addresses or samplers decide')
            if problems:
                skipped.append((k['key'], '; '.join(problems)))
                continue
            attach_residuals(k, bound)
            constexpr_tsc[:] = samplers
            seen.add(k['key'])
            kernels.append(k)
    return kernels, skipped


def lay_out(kernels: list[dict], programs: dict[str, dict], heap: bytearray, prog_index: dict, prog_recs: list) -> None:
    """Append each kernel's program to the heap image and set each kernel's program index.

    Its code starts its slot (no shader header: NVK's compute upload), its shader data 256-aligned after.
    """
    for k in kernels:
        pid = k.pop('prog')
        if pid not in prog_index:
            pr = programs[pid]
            heap += bytes((-len(heap)) % 0x100)
            off = len(heap)
            slot = bytearray(pr['image'][80:])
            slot += bytes((-len(slot)) % 0x100)
            data_offset = len(slot) if pr['data'] else 0
            slot += pr['data']
            heap += slot
            prog_index[pid] = len(prog_recs)
            prog_recs.append(
                {
                    'id': pid,
                    'heap_offset': off,
                    'program': off,
                    'bytes': len(slot),
                    'code': len(pr['image']) - 80,
                    'cbufs': pr['cbufs'],
                    'data_offset': data_offset,
                    'data_bytes': len(pr['data']),
                }
            )
        k['program'] = prog_index[pid]
