"""The flight recorder's dumps in a pulled crash folder: each ring read with the stackshot taken beside it, as text.

A dump is Switch-recorder-<slot>.ktrace and .kcdata (kernel/switch/recorder/recorder.c). The stackshot names the
ring's threads (process, thread name) and holds every process's loaded images, which turn kperf's user addresses into
`dsc 0x<unslid>` for the shared cache and `<image>+0x<offset>` for the rest (or symbols when a table is supplied);
kcdata.py, XNU's own decoder (tools/lldbmacros), reads it.
"""

from __future__ import annotations

import bisect
import json
import os
import subprocess
import sys
from collections import Counter
from dataclasses import dataclass, field

from tools.perf import ktrace, symbols
from tools.perf.source import TICK_HZ

BEFORE_S = 1.0  # what the summary looks back over from the trigger
GPU_BEFORE_S = 0.2
FRAME_BEFORE_S = 0.1
DSC_SPAN = 0x200000000  # a shared cache's mapped size is well under 8 GiB
IMAGE_SPAN = 256 << 20
TOP = 8


@dataclass
class Names:
    """What a stackshot says of each thread and each process's images."""

    thread: dict[int, tuple[int, str, str]] = field(default_factory=dict)  # tid: (pid, process, thread name)
    images: dict[int, list[tuple[int, str]]] = field(default_factory=dict)  # pid: [(load address, uuid)], sorted
    dsc: dict[int, tuple[int, int]] = field(default_factory=dict)  # pid: (slid base, slide)

    def who(self, tid: int) -> str:
        pid, proc, name = self.thread.get(tid, (0, '?', ''))
        return f'{proc}[{pid}]' + (f' {name}' if name else '') + f' ({tid})'

    def where(
        self,
        tid: int,
        pc: int,
        uuid_paths: dict[str, str],
        syms: symbols.Symbols | None = None,
    ) -> str:
        pid = self.thread.get(tid, (0, '', ''))[0]
        if pid in self.dsc:
            base, slide = self.dsc[pid]
            if base <= pc < base + DSC_SPAN:
                unslid = pc - slide
                return syms.dsc_addr(unslid) if syms else f'dsc 0x{unslid:x}'
        loads = self.images.get(pid, [])
        i = bisect.bisect_right(loads, (pc, '\xff')) - 1
        if i >= 0 and pc - loads[i][0] < IMAGE_SPAN:
            uuid = loads[i][1]
            name = uuid_paths.get(uuid.upper(), '').rsplit('/', 1)[-1] or uuid[:8]
            off = pc - loads[i][0]
            return syms.image_addr(name, off, uuid) if syms else f'{name}+0x{off:x}'
        return f'0x{pc:x}'


def uuid_text(raw: object) -> str:
    if isinstance(raw, list):
        h = bytes(raw).hex()
        return f'{h[:8]}-{h[8:12]}-{h[12:16]}-{h[16:20]}-{h[20:]}'
    return str(raw)


def names(kcdata: dict) -> Names:
    out = Names()
    shot = kcdata.get('kcdata_stackshot') or {}
    tasks = {**(shot.get('task_snapshots') or {}), **(shot.get('transitioning_task_snapshots') or {})}
    for task in tasks.values():
        snap = task.get('task_snapshot') or {}
        pid = int(snap.get('ts_pid', -1))
        for thread in (task.get('thread_snapshots') or {}).values():
            tid = int((thread.get('thread_snapshot') or {}).get('ths_thread_id', 0))
            out.thread[tid] = (pid, snap.get('ts_p_comm', '?'), thread.get('pth_name', ''))
        loads = task.get('dyld_load_info') or []
        out.images[pid] = sorted((int(i['imageLoadAddress']), uuid_text(i.get('imageUUID'))) for i in loads)
        sc = task.get('shared_cache_dyld_load_info')
        if sc:
            out.dsc[pid] = (int(sc.get('imageSlidBaseAddress', 0)), int(sc.get('imageLoadAddress', 0)))
    return out


def decode_kcdata(path: str, kcdata_py: str | None) -> dict:
    if not kcdata_py or not os.path.isfile(kcdata_py):
        return {}
    done = subprocess.run([sys.executable, '-I', kcdata_py, path], capture_output=True, text=True, check=False)
    try:
        return json.loads(done.stdout)
    except json.JSONDecodeError:
        return {}


def find(crashes: str | None) -> list[tuple[str, str]]:
    """Return each dump's (ring, stackshot) paths, the newest last."""
    if not crashes:
        return []
    found = []
    for parent, _, files in os.walk(crashes):
        for f in files:
            if f.startswith('Switch-recorder-') and f.endswith('.ktrace'):
                ring = os.path.join(parent, f)
                found.append((os.path.getmtime(ring), ring, ring[: -len('.ktrace')] + '.kcdata'))
    return [(ring, shot) for _, ring, shot in sorted(found)]


def ms(ticks: int) -> float:
    return ticks * 1000 / TICK_HZ


def busiest(ring: ktrace.Ring, n: Names, lo: int, hi: int) -> list[str]:
    stretches = ktrace.runs(ring.events)
    used = sorted(((ktrace.on_core(s, lo, hi), tid) for tid, s in stretches.items()), reverse=True)
    return [f'{n.who(tid)} {ms(t):.0f} ms' for t, tid in used[:TOP] if t and tid]


def stacks(
    ring: ktrace.Ring,
    n: Names,
    tid: int,
    uuid_paths: dict[str, str],
    syms: symbols.Symbols | None = None,
) -> list[str]:
    user = [s for s in ktrace.samples(ring.events) if s.tid == tid and not s.kernel and s.frames]
    if not user:
        return ['no user stack samples of it in the ring']
    leaves = Counter(n.where(tid, s.frames[0], uuid_paths, syms) for s in user)
    whole = Counter(tuple(n.where(tid, f, uuid_paths, syms) for f in s.frames[:6]) for s in user)
    top, count = whole.most_common(1)[0]
    return [
        f'{len(user)} user stack samples; leaves: ' + ', '.join(f'{k} {v}' for k, v in leaves.most_common(TOP)),
        f'most common stack ({count}): ' + ' < '.join(top),
    ]


def gpu(ring: ktrace.Ring, n: Names, lo: int, hi: int) -> list[str]:
    done, submitter = ktrace.jobs(ring.events)
    inside = [j for j in done if lo <= j.ts <= hi]
    if not inside:
        return ['GPU: no job ended in the window']
    by: Counter[str] = Counter()
    for j in inside:
        by[n.who(submitter.get(j.channel, 0)).split(' (')[0]] += max(0, j.end_ns - j.start_ns) if j.start_ns else 0
    longest = max(inside, key=lambda j: j.end_ns - j.start_ns if j.start_ns else 0)
    longest_ms = (longest.end_ns - longest.start_ns) / 1e6 if longest.start_ns else 0
    shares = ', '.join(f'{k} {v / 1e6:.1f} ms' for k, v in by.most_common(TOP))
    kind = ' (compositor)' if longest.compositor else ''
    return [
        f'GPU: {len(inside)} jobs ended; GPU time by process: {shares}',
        f'GPU: longest job channel {longest.channel} seqno {longest.seqno}{kind} {longest_ms:.1f} ms',
    ]


def pagein_lines(ring: ktrace.Ring, n: Names, lo: int, hi: int) -> list[str]:
    disk_ios, fs_pageins = ktrace.pageins(ring.events)
    dk_in = [io for io in disk_ios if lo <= io.ts <= hi and (io.paging or io.read)]
    fs_in = [p for p in fs_pageins if lo <= p.ts <= hi]
    if not dk_in and not fs_in:
        return []
    paging_dk = [io for io in dk_in if io.paging]
    parts = []
    if fs_in:
        parts.append(f'{len(fs_in)} FSRW page-ins ({sum(p.nbytes for p in fs_in) // 1024} KiB)')
    if paging_dk:
        kib = sum(io.nbytes for io in paging_dk) // 1024
        tot_ms = ms(sum(io.duration for io in paging_dk))
        worst_ms = ms(max(io.duration for io in paging_dk))
        parts.append(f'{len(paging_dk)} DKRW paging reads ({kib} KiB, {tot_ms:.1f} ms total, max {worst_ms:.1f} ms)')
    elif dk_in:
        kib = sum(io.nbytes for io in dk_in) // 1024
        parts.append(f'{len(dk_in)} DKRW reads ({kib} KiB, {ms(sum(io.duration for io in dk_in)):.1f} ms)')
    out = ['page-ins: ' + '; '.join(parts)]
    by_file: Counter[str] = Counter()
    for io in paging_dk or dk_in:
        label = io.path or (f'vnode 0x{io.vnode:x}' if io.vnode else '(unnamed)')
        by_file[label] += io.nbytes or 1
    if by_file:
        out.append('page-in files: ' + ', '.join(f'{f} {b // 1024} KiB' for f, b in by_file.most_common(TOP)))
    elif fs_in:
        by_thr: Counter[str] = Counter()
        for p in fs_in:
            by_thr[n.who(p.tid)] += p.nbytes
        out.append('page-in threads: ' + ', '.join(f'{t} {b // 1024} KiB' for t, b in by_thr.most_common(4)))
    return out


def unnest_lines(ring: ktrace.Ring, n: Names, lo: int, hi: int) -> list[str]:
    uns, layouts = ktrace.unnests(ring.events)
    u_in = [u for u in uns if lo <= u.ts <= hi]
    l_in = [lay for lay in layouts if lo <= lay.ts <= hi]
    if not u_in and not l_in:
        return []
    out = []
    if u_in:
        total_kib = sum(u.size for u in u_in) // 1024
        items = []
        for u in u_in[:TOP]:
            dur = f' ({ms(u.syscall_ticks):.1f} ms)' if u.syscall_ticks else ''
            sc = f' in {u.syscall}{dur}' if u.syscall else ''
            items.append(f'{n.who(u.tid)} {u.size // 1024} KiB{sc}')
        out.append(f'shared-cache unnests: {len(u_in)} ({total_kib} KiB): ' + ', '.join(items))
    if l_in:
        tot_ms = ms(sum(lay.duration for lay in l_in))
        out.append(f'shared-cache layout: {len(l_in)} maps ({tot_ms:.1f} ms)')
    return out


def frame_points(ring: ktrace.Ring, lo: int, hi: int) -> str:
    points = [
        (e.ts, ktrace.FRAME_POINTS[e.id]) for e in ring.events if e.id in ktrace.FRAME_POINTS and lo <= e.ts <= hi
    ]
    if not points:
        return 'frame points: none in the window'
    return 'frame points (ms before the trigger): ' + ', '.join(
        f'{name} {ms(ts - hi):+.1f}' for ts, name in points[-24:]
    )


def summary(
    ring_path: str,
    shot_path: str,
    kcdata_py: str | None,
    uuid_paths: dict[str, str],
    syms: symbols.Symbols | None = None,
) -> list[str]:
    ring = ktrace.read(ring_path)
    n = names(decode_kcdata(shot_path, kcdata_py)) if os.path.isfile(shot_path) else Names()
    if not ring.events:
        return [f'{os.path.basename(ring_path)}: an empty ring']
    first, last = ring.events[0].ts, ring.events[-1].ts
    found = ktrace.trigger(ring.events)
    at, reason, tid, value = found or (last, 'none', 0, 0)
    lo = at - int(BEFORE_S * TICK_HZ)
    out = [
        f'{os.path.basename(ring_path)}: {reason}, thread {n.who(tid) if tid else "-"}, value {value}; ring '
        f'{ms(last - first) / 1000:.2f} s, {len(ring.events)} events, trigger at {ms(at - first) / 1000:.2f} s'
        + ('' if n.thread else '; no stackshot read (pass --kcdata, XNU tools/lldbmacros/kcdata.py)'),
        '  threads by time on a core in the second before it: ' + ', '.join(busiest(ring, n, lo, at)),
    ]
    if tid:
        held = ktrace.on_core(ktrace.runs(ring.events).get(tid, []), lo, at)
        woken = ktrace.wakers(ring.events, tid, lo, at)
        out.append(
            f'  its thread: on a core {ms(held):.0f} ms of the last {BEFORE_S * 1000:.0f}; woken by '
            + (', '.join(f'{n.who(w)} {c}' for w, c in sorted(woken.items(), key=lambda kv: -kv[1])[:4]) or 'nobody')
        )
        out += ['  ' + line for line in stacks(ring, n, tid, uuid_paths, syms)]
    out += ['  ' + line for line in pagein_lines(ring, n, lo, at)]
    out += ['  ' + line for line in unnest_lines(ring, n, lo, at)]
    out += ['  ' + line for line in gpu(ring, n, at - int(GPU_BEFORE_S * TICK_HZ), at)]
    out.append('  ' + frame_points(ring, at - int(FRAME_BEFORE_S * TICK_HZ), at))
    return out
