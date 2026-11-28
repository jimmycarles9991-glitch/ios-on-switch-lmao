"""The flight recorder's dumps in a pulled crash folder: each ring read with the stackshot taken beside it, as text.

A dump is one Switch-recorder-<slot>.rec (recfile.py), or from kernels before 26 Nov a Switch-recorder-<slot>.ktrace
and .kcdata pair, which a later dump in the same slot could leave mismatched. The stackshot names the
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
import tempfile
from collections import Counter
from dataclasses import dataclass, field

from tools.perf import ktrace, recfile, symbols
from tools.perf.source import TICK_HZ

BEFORE_S = 1.0  # what the summary looks back over from the trigger
GPU_BEFORE_S = 0.2
FRAME_BEFORE_S = 0.1
DSC_SPAN = 0x200000000  # a shared cache's mapped size is well under 8 GiB
IMAGE_SPAN = 256 << 20
TOP = 8
# kcdata.h's ts_ss_flags: which shared region a task is attached to
REGION_NONE, REGION_SYSTEM, REGION_OTHER = 0x20000000, 0x40000000, 0x80000000
# osfmk/kern/block_hint.h's kThreadWait*, and kcdata.h's waitinfo and turnstile flags
WAIT_TYPES = {
    1: 'a kernel mutex',
    2: 'a port receive',
    3: 'a port set receive',
    4: 'a port send',
    6: 'a semaphore',
    10: 'a user lock',
    11: 'a pthread mutex',
    14: 'a condition variable',
    16: 'a workloop sync wait',
    18: 'a sleep with inheritor',
    20: 'the compressor',
    22: 'a busy page',
    25: 'paging activity',
}
WAIT_SPECIAL_REPLY = 0x1
TURNSTILE_THREAD, TURNSTILE_TASK, TURNSTILE_WORKQUEUE = 0x08, 0x10, 0x04


@dataclass
class Names:
    """What a stackshot says of each thread and each process's images."""

    thread: dict[int, tuple[int, str, str]] = field(default_factory=dict)  # tid: (pid, process, thread name)
    images: dict[int, list[tuple[int, str]]] = field(default_factory=dict)  # pid: [(load address, uuid)], sorted
    dsc: dict[int, tuple[int, int]] = field(default_factory=dict)  # pid: (slid base, slide)
    waits: dict[int, tuple[int, int]] = field(default_factory=dict)  # tid: (wait type, waitinfo flags)
    turnstiles: dict[int, tuple[int, int, int]] = field(default_factory=dict)  # tid: (inheritor, flags, hops)
    procs: dict[int, str] = field(default_factory=dict)  # pid: process

    def waited(self, tid: int) -> str:
        """Return what the thread waited on at the stackshot, and behind whom its turnstile ended."""
        if tid not in self.waits:
            return ''
        kind, flags = self.waits[tid]
        text = WAIT_TYPES.get(kind, f'wait type {kind}') + (' (its reply port)' if flags & WAIT_SPECIAL_REPLY else '')
        if tid in self.turnstiles:
            ctx, tflags, hops = self.turnstiles[tid]
            if tflags & TURNSTILE_THREAD and ctx:
                text += f', behind {self.who(ctx)} ({hops} hops)'
            elif tflags & TURNSTILE_TASK and ctx:
                text += f', behind {self.procs.get(ctx, "?")}[{ctx}] ({hops} hops)'
            elif tflags & TURNSTILE_WORKQUEUE:
                text += f', behind a workqueue ({hops} hops)'
        return text

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


def cache_base_slide(info: dict | None) -> tuple[int, int] | None:
    """Return a shared cache's (slid base, slide) from either of kcdata's load-info layouts."""
    if not info:
        return None
    if 'sharedCacheSlide' in info:
        return int(info.get('sharedCacheSlidFirstMapping', 0)), int(info['sharedCacheSlide'])
    return int(info.get('imageSlidBaseAddress', 0)), int(info.get('imageLoadAddress', 0))


Cache = tuple[int, int]  # (slid base, slide)


def task_cache(task: dict, system: Cache | None, others: dict[int, Cache]) -> Cache | None:
    """Return the shared cache a task maps.

    Older stackshots carry it in every task. Newer ones carry the system cache once at the top level and give a task
    only its region flags, plus a cache id when it maps another one.
    """
    own = cache_base_slide(task.get('shared_cache_dyld_load_info'))
    if own:
        return own
    flags = int((task.get('task_snapshot') or {}).get('ts_ss_flags', 0))
    if flags & REGION_OTHER:
        return others.get(int(task.get('sharedCacheID', 0)))
    if flags & REGION_NONE:
        return None
    return system


def names(kcdata: dict) -> Names:
    out = Names()
    shot = kcdata.get('kcdata_stackshot') or {}
    tasks = {**(shot.get('task_snapshots') or {}), **(shot.get('transitioning_task_snapshots') or {})}
    system = cache_base_slide(shot.get('shared_cache_dyld_load_info'))
    others = {}
    for entry in (shot.get('shared_caches') or {}).values():
        info = entry.get('shared_cache_dyld_load_info') or {}
        if (found := cache_base_slide(info)) and 'sharedCacheID' in info:
            others[int(info['sharedCacheID'])] = found
    for task in tasks.values():
        snap = task.get('task_snapshot') or {}
        pid = int(snap.get('ts_pid', -1))
        for thread in (task.get('thread_snapshots') or {}).values():
            tid = int((thread.get('thread_snapshot') or {}).get('ths_thread_id', 0))
            out.thread[tid] = (pid, snap.get('ts_p_comm', '?'), thread.get('pth_name', ''))
        out.procs[pid] = snap.get('ts_p_comm', '?')
        for w in task.get('thread_waitinfo') or []:
            out.waits[int(w.get('waiter', 0))] = (int(w.get('wait_type', 0)), int(w.get('wait_flags', 0)))
        for t in task.get('thread_turnstileinfo') or []:
            out.turnstiles[int(t.get('waiter', 0))] = (
                int(t.get('turnstile_context', 0)),
                int(t.get('turnstile_flags', 0)),
                int(t.get('number_of_hops', 0)),
            )
        loads = task.get('dyld_load_info') or []
        out.images[pid] = sorted((int(i['imageLoadAddress']), uuid_text(i.get('imageUUID'))) for i in loads)
        if cache := task_cache(task, system, others):
            out.dsc[pid] = cache
    return out


def decode_kcdata(path: str, kcdata_py: str | None) -> dict:
    if not kcdata_py or not os.path.isfile(kcdata_py):
        return {}
    done = subprocess.run([sys.executable, '-I', kcdata_py, path], capture_output=True, text=True, check=False)
    try:
        return json.loads(done.stdout)
    except json.JSONDecodeError:
        return {}


def decode_shot(shot: bytes, kcdata_py: str | None) -> dict:
    with tempfile.NamedTemporaryFile(suffix='.kcdata') as f:
        f.write(shot)
        f.flush()
        return decode_kcdata(f.name, kcdata_py)


def find(crashes: str | None) -> list[tuple[str, str]]:
    """Return each dump's (ring, stackshot) paths, the newest last: a .rec file names itself twice."""
    if not crashes:
        return []
    found = []
    for parent, _, files in os.walk(crashes):
        for f in files:
            path = os.path.join(parent, f)
            if f.startswith('Switch-recorder-') and f.endswith('.ktrace'):
                found.append((os.path.getmtime(path), path, path[: -len('.ktrace')] + '.kcdata'))
            elif f.startswith('Switch-recorder-') and f.endswith('.rec'):
                found.append((os.path.getmtime(path), path, path))
    return [(ring, shot) for _, ring, shot in sorted(found)]


def load(ring_path: str, shot_path: str, kcdata_py: str | None) -> tuple[ktrace.Ring, Names, recfile.Dump | None]:
    """Return a dump's ring, its threads' names, and its .rec file's header (None for a .ktrace pair).

    The stackshot names the threads alive when it was taken; the ring's own new-thread and name events name those that
    ended before it.
    """
    dump = None
    if ring_path.endswith('.rec'):
        dump = recfile.read(ring_path)
        if dump.problem:
            raise ValueError(dump.problem)
        n = names(decode_shot(dump.shot, kcdata_py)) if dump.shot else Names()
        ring = ktrace.parse(dump.ring) if dump.ring else ktrace.Ring(0, [])
    else:
        n = names(decode_kcdata(shot_path, kcdata_py)) if os.path.isfile(shot_path) else Names()
        ring = ktrace.read(ring_path)
    for tid, named in ktrace.thread_names(ring.events).items():
        n.thread.setdefault(tid, named)
    return ring, n, dump


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
    """Return the GPU's submissions that ended in [lo, hi], their time by process, and the longest.

    A submission's time runs from its first batch's start to its end, other channels' slices inside it included.
    """
    span_ms = ms(ring.events[-1].ts - ring.events[0].ts) if ring.events else 0.0
    ended = [s for s in ktrace.submissions(ring.events) if lo <= s.ts <= hi]
    inside = [s for s in ended if s.ms <= span_ms]
    impossible = Counter(s.channel for s in ended if s.ms > span_ms)
    note = (
        [
            'GPU: left out, a start stamp older than the ring: '
            + ', '.join(f'channel {c} {n}' for c, n in impossible.items())
        ]
        if impossible
        else []
    )
    note += wait_lines(ring, n, lo, hi) + room_lines(ring, n, lo, hi)
    if not inside:
        return ['GPU: no submission ended in the window', *note]
    by: Counter[str] = Counter()
    for s in inside:
        by[n.who(s.tid).split(' (')[0]] += s.ms
    longest = max(inside, key=lambda s: s.ms)
    shares = ', '.join(f'{k} {v:.1f} ms' for k, v in by.most_common(TOP))
    if bounded := Counter(s.channel for s in inside if s.bounded):
        note.append(
            "GPU: from their channel's previous end, a stale start stamp: "
            + ', '.join(f'channel {c} {k}' for c, k in bounded.items())
        )
    kind = ' (compositor)' if longest.compositor else ''
    seqnos = f'{longest.first}-{longest.last}' if longest.last != longest.first else f'{longest.last}'
    return [
        f'GPU: {len(inside)} submissions ended; GPU time by process: {shares}',
        f'GPU: longest submission channel {longest.channel} seqno {seqnos}{kind} {longest.ms:.1f} ms, by '
        + n.who(longest.tid),
        *note,
    ]


def wait_lines(ring: ktrace.Ring, n: Names, lo: int, hi: int) -> list[str]:
    """Return the compositor's waits in [lo, hi] on another process's GPU work or another engine, by whom."""
    waits = [w for w in ktrace.gpu_waits(ring.events) if lo <= w.ts <= hi]
    if not waits:
        return []
    owner = {e.args[0]: e.tid for e in ring.events if e.id == ktrace.SUBMIT}
    by: Counter[str] = Counter()
    for w in waits:
        on = f'channel {w.source} ({n.who(owner.get(w.source, 0)).split(" (")[0]})' if w.source else w.engine
        by[f'channel {w.channel} on {on}'] += 1
    return ['GPU waits behind unsettled work: ' + ', '.join(f'{k} {v}' for k, v in by.most_common(TOP))]


def sd_lines(ring: ktrace.Ring, lo: int, hi: int) -> list[str]:
    """Return the SD queue's picks in [lo, hi]: by class and kind, how many passed older requests, how deep it ran."""
    picks = [p for p in ktrace.sd_picks(ring.events) if lo <= p.ts <= hi]
    if not picks:
        return []
    by = Counter(f'{"write" if p.write else "read"} {ktrace.SD_CLASSES[p.klass & 3]}' for p in picks)
    return [
        f'SD queue: {len(picks)} picks (' + ', '.join(f'{k} {v}' for k, v in sorted(by.items())) + '), '
        f'{sum(p.passed for p in picks)} passed older requests; up to {max(p.queued for p in picks)} queued, the head '
        f'waiting up to {max(p.head_waited_us for p in picks) / 1000:.1f} ms'
    ]


def room_lines(ring: ktrace.Ring, n: Names, lo: int, hi: int) -> list[str]:
    """Return the submissions that waited for room in their channel's ring in [lo, hi], the longest first."""
    waits = sorted(
        (w for w in ktrace.room_waits(ring.events) if w.end >= lo and w.start <= hi), key=lambda w: w.start - w.end
    )
    if not waits:
        return []
    return [
        f'GPU ring full: {len(waits)} submissions waited for room, '
        + ', '.join(
            f'{n.who(w.tid)} channel {w.channel} {ms(w.end - w.start):.1f} ms (GET {w.get} PUT {w.put}, '
            f'{w.unfinished} jobs unfinished{", timed out" if w.timed_out else ""})'
            for w in waits[:4]
        )
    ]


def pagein_lines(ring: ktrace.Ring, n: Names, lo: int, hi: int) -> list[str]:
    disk_ios, fs_pageins = ktrace.pageins(ring.events)
    dk_in = [io for io in disk_ios if lo <= io.ts <= hi and (io.paging or io.read)]
    fs_in = [p for p in fs_pageins if lo <= p.ts <= hi]
    if not dk_in and not fs_in:
        return []
    paging_dk = [io for io in dk_in if io.paging]
    tiers = Counter(f'{io.tier}{"p" if io.passive else ""}' for io in paging_dk or dk_in)
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
    out = [
        'page-ins: '
        + '; '.join(parts)
        + '; by tier (p: passive): '
        + ', '.join(f'{k} {v}' for k, v in sorted(tiers.items()))
    ]
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


def fault_lines(ring: ktrace.Ring, n: Names, tid: int, lo: int, hi: int) -> list[str]:
    """Return the thread's own fault time in [lo, hi] by kind, and the disk reads its page-ins waited behind.

    A read in flight while the thread was in a page-in fault, its own or another thread's, is one it waited for or
    queued behind on the card; its issuer and tier say whose fill it joined and at what priority.
    """
    mine = [f for f in ktrace.faults(ring.events) if f.tid == tid and f.end >= lo and f.start <= hi]
    if not mine:
        return []
    by: Counter[str] = Counter()
    for f in mine:
        by[f.kind] += f.end - f.start
    out = [
        f'its faults: {len(mine)}, '
        + ', '.join(f'{k} {ms(t):.1f} ms' for k, t in by.most_common())
        + f'; the longest {ms(max(f.end - f.start for f in mine)):.1f} ms'
    ]
    pageins = [f for f in mine if f.kind == 'page-in']
    reads = [
        io
        for io in ktrace.pageins(ring.events)[0]
        if io.read and any(io.ts - io.duration <= f.end and io.ts >= f.start for f in pageins)
    ]
    if reads:
        issuers: Counter[str] = Counter()
        for io in reads:
            who = 'its own' if io.tid == tid else n.who(io.tid)
            issuers[f'{who} tier {io.tier}{" passive" if io.passive else ""}'] += io.nbytes
        out.append(
            f'disk reads during its page-ins: {len(reads)}, {sum(io.nbytes for io in reads) // 1024} KiB, '
            f'{ms(max(io.duration for io in reads)):.1f} ms the longest; by issuer: '
            + ', '.join(f'{k} {b // 1024} KiB' for k, b in issuers.most_common(TOP))
        )
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
    ring, n, dump = load(ring_path, shot_path, kcdata_py)
    base = os.path.basename(ring_path)
    if not ring.events:
        return [f'{base}: ' + (dump.describe() + '; ' if dump else '') + 'an empty ring']
    first, last = ring.events[0].ts, ring.events[-1].ts
    if dump:
        at, reason, tid, value = (
            dump.number('at'),
            dump.head.get('reason', '?'),
            dump.number('tid'),
            dump.number('value'),
        )
    else:
        at, reason, tid, value = ktrace.trigger(ring.events) or (last, 'none', 0, 0)
    lo = max(first, at - int(BEFORE_S * TICK_HZ))
    out = [
        f'{base}: {reason}, thread {n.who(tid) if tid else "-"}, value {value}; ring '
        f'{ms(last - first) / 1000:.2f} s, {len(ring.events)} events, trigger at {ms(at - first) / 1000:.2f} s'
        + ('' if n.thread else '; no stackshot read (pass --kcdata, XNU tools/lldbmacros/kcdata.py)'),
    ]
    if dump:
        out.append('  ' + dump.describe())
    if at - lo < BEFORE_S * TICK_HZ:
        before = f'capture {dump.number("seq") - 1}' if dump and dump.number('seq') else 'the capture before it'
        out.append(f'  the ring reaches {ms(at - lo):.0f} ms before the trigger: {before} read the rest')
    out.append('  threads by time on a core in the second before it: ' + ', '.join(busiest(ring, n, lo, at)))
    if tid:
        held = ktrace.on_core(ktrace.runs(ring.events).get(tid, []), lo, at)
        woken = ktrace.wakers(ring.events, tid, lo, at)
        out.append(
            f'  its thread: on a core {ms(held):.0f} ms of the last {ms(at - lo):.0f}; woken by '
            + (', '.join(f'{n.who(w)} {c}' for w, c in sorted(woken.items(), key=lambda kv: -kv[1])[:4]) or 'nobody')
        )
        if waited := n.waited(tid):
            out.append(f'  at the stackshot it waited on {waited}')
        out += ['  ' + line for line in stacks(ring, n, tid, uuid_paths, syms)]
        out += ['  ' + line for line in fault_lines(ring, n, tid, lo, at)]
    out += ['  ' + line for line in pagein_lines(ring, n, lo, at)]
    out += ['  ' + line for line in sd_lines(ring, lo, at)]
    out += ['  ' + line for line in unnest_lines(ring, n, lo, at)]
    out += ['  ' + line for line in gpu(ring, n, at - int(GPU_BEFORE_S * TICK_HZ), at)]
    out.append('  ' + frame_points(ring, at - int(FRAME_BEFORE_S * TICK_HZ), at))
    return out
