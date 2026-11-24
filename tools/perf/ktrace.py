"""The flight recorder's ring (kernel/switch/recorder): a dump's RAW kdebug file as events, and what the events say.

kdbg_dump_trace_to_file writes a RAW_VERSION1 header, a thread map (none here: the stackshot beside the file names
the threads), padding to a 4 KiB boundary that carries the CPU map, then 64-byte events oldest first: the mach time,
four arguments, the thread that logged it, the debug id and the CPU.
"""

from __future__ import annotations

import struct
from collections import defaultdict
from dataclasses import dataclass, field

RAW_VERSION1 = 0x55AA0101
HEADER = struct.Struct('<IiQI4x')
THREADMAP_ENTRY = 32
EVENT = struct.Struct('<QQQQQQIIQ')
PAGE = 4096
FUNC_START, FUNC_END = 1, 2


def code(cls: int, sub: int, number: int) -> int:
    return (cls << 24) | (sub << 16) | (number << 2)


# bsd/sys/kdebug.h, osfmk/kperf/buffer.h, kernel/switch/include/switch/recorder.h and
# TegraDisplayFlip/SwitchFrameTimeline.h
MACH_SCHED = code(1, 0x40, 0)
MACH_MAKE_RUNNABLE = code(1, 0x40, 6)
VM_PMAP_UNNEST = code(1, 0x30, 0x161)
SHARED_CACHE_LAYOUT = code(1, 0xA8, 0)
VFS_LOOKUP = code(3, 1, 36)
VFS_LOOKUP_DONE = code(3, 1, 39)
CL_PAGEIN = code(3, 1, 56)
DBG_DKRW_PREFIX = (3 << 8) | 2
DBG_BSD_SC_PREFIX = (4 << 8) | 0x0C
DBG_MACH_SC_PREFIX = (1 << 8) | 0x0C
DKIO_DONE = 0x01
DKIO_READ = 0x02
DKIO_ASYNC = 0x04
DKIO_META = 0x08
DKIO_PAGING = 0x10
PERF_TI_DATA = code(37, 1, 1)
PERF_CS_KDATA, PERF_CS_UDATA = code(37, 2, 3), code(37, 2, 4)
PERF_CS_KHDR, PERF_CS_UHDR = code(37, 2, 5), code(37, 2, 6)
TRIGGER, DUMP, GPU_JOB = code(6, 18, 0x3E0), code(6, 18, 0x3E1), code(6, 18, 0x3E8)
SUBMIT = code(6, 18, 0x3F1)
FRAME_POINTS = {
    code(6, 18, 0x3F0 + i): name
    for i, name in enumerate(
        ('vsync', 'submit', 'gpu done', 'swap', 'present', 'present done', 'flip', 'latch', 'frame ticket')
    )
}
REASONS = ('none', 'late frame', 'hang', 'slow work', 'mark')
BSD_SYSCALLS = {
    73: 'munmap',
    74: 'mprotect',
    197: 'mmap',
    294: 'shared_region_check_np',
    295: 'shared_region_map_np',
    436: 'shared_region_map_and_slide_np',
    538: 'shared_region_map_and_slide_2_np',
}
MACH_TRAPS = {
    10: 'mach_vm_allocate',
    12: 'mach_vm_deallocate',
    14: 'mach_vm_protect',
    15: 'mach_vm_map',
}


@dataclass(frozen=True)
class Event:
    ts: int
    args: tuple[int, int, int, int]
    tid: int
    debugid: int
    cpu: int

    @property
    def id(self) -> int:
        return self.debugid & ~3

    @property
    def func(self) -> int:
        return self.debugid & 3


@dataclass
class Ring:
    tod_secs: int
    events: list[Event]


@dataclass
class Sample:
    ts: int
    tid: int
    kernel: bool
    frames: list[int] = field(default_factory=list)


@dataclass(frozen=True)
class Job:
    ts: int
    channel: int
    seqno: int
    compositor: bool
    start_ns: int
    end_ns: int


@dataclass(frozen=True)
class DiskIO:
    ts: int
    tid: int
    duration: int
    nbytes: int
    vnode: int
    path: str
    read: bool
    paging: bool
    meta: bool


@dataclass(frozen=True)
class PageIn:
    ts: int
    tid: int
    nbytes: int
    offset: int


@dataclass
class Unnest:
    ts: int
    tid: int
    start: int
    end: int
    syscall: str = ''
    syscall_ticks: int = 0

    @property
    def size(self) -> int:
        return max(0, self.end - self.start)


@dataclass(frozen=True)
class SharedLayout:
    ts: int
    tid: int
    duration: int
    images: int


def parse(data: bytes) -> Ring:
    if len(data) < HEADER.size:
        raise ValueError(f'kdebug file shorter than RAW_VERSION1 header ({len(data)} bytes)')
    version, threads, secs, _usecs = HEADER.unpack_from(data, 0)
    if version != RAW_VERSION1:
        raise ValueError(f'not a RAW_VERSION1 kdebug file (version 0x{version & 0xFFFFFFFF:x})')
    if threads < 0:
        raise ValueError(f'invalid RAW_VERSION1 thread count ({threads})')
    start = -(-(HEADER.size + threads * THREADMAP_ENTRY) // PAGE) * PAGE
    if start > len(data):
        raise ValueError(f'RAW_VERSION1 header extends past file ({start} > {len(data)})')
    end = start + (len(data) - start) // EVENT.size * EVENT.size
    events = [
        Event(ts, (a1, a2, a3, a4), tid, debugid, cpu)
        for ts, a1, a2, a3, a4, tid, debugid, cpu, _ in EVENT.iter_unpack(data[start:end])
        if ts
    ]
    events.sort(key=lambda e: e.ts)
    return Ring(secs, events)


def tod(path: str) -> int:
    """Return the wall time (s) a dump's ring was written, from its header alone (0 if unreadable or wrong magic)."""
    try:
        with open(path, 'rb') as f:
            head = f.read(HEADER.size)
    except OSError:
        return 0
    if len(head) != HEADER.size:
        return 0
    version, threads, secs, _ = HEADER.unpack(head)
    return secs if version == RAW_VERSION1 and threads >= 0 else 0


def read(path: str) -> Ring:
    with open(path, 'rb') as f:
        return parse(f.read())


def runs(events: list[Event]) -> dict[int, list[tuple[int, int, int]]]:
    """Return each thread's stretches on a core, (start, end, cpu), from the context switches."""
    on: dict[int, tuple[int, int]] = {}  # cpu: (thread, since)
    out: dict[int, list[tuple[int, int, int]]] = defaultdict(list)
    first = events[0].ts if events else 0
    for e in events:
        if e.id != MACH_SCHED:
            continue
        old_tid, since = on.get(e.cpu, (e.tid, first))
        out[old_tid].append((since, e.ts, e.cpu))
        on[e.cpu] = (e.args[1], e.ts)
    last = events[-1].ts if events else 0
    for cpu, (tid, since) in on.items():
        out[tid].append((since, last, cpu))
    return out


def on_core(stretches: list[tuple[int, int, int]], lo: int, hi: int) -> int:
    return sum(max(0, min(b, hi) - max(a, lo)) for a, b, _ in stretches)


def wakers(events: list[Event], tid: int, lo: int, hi: int) -> dict[int, int]:
    """Return who made the thread runnable in [lo, hi], and how often."""
    out: dict[int, int] = defaultdict(int)
    for e in events:
        if e.id == MACH_MAKE_RUNNABLE and e.args[0] == tid and lo <= e.ts <= hi:
            out[e.tid] += 1
    return out


def samples(events: list[Event]) -> list[Sample]:
    """Return kperf's stack samples: a header with the frame count, then its frames four to an event."""
    pending: dict[tuple[int, bool], tuple[Sample, int]] = {}
    out = []
    for e in events:
        if e.id in {PERF_CS_KHDR, PERF_CS_UHDR}:
            kernel = e.id == PERF_CS_KHDR
            pending[(e.tid, kernel)] = (Sample(e.ts, e.tid, kernel), e.args[1])
        elif e.id in {PERF_CS_KDATA, PERF_CS_UDATA}:
            key = (e.tid, e.id == PERF_CS_KDATA)
            if key not in pending:
                continue
            sample, want = pending[key]
            sample.frames += [f for f in e.args if f][: want - len(sample.frames)]
            if len(sample.frames) >= want or not all(e.args):
                out.append(sample)
                del pending[key]
    return out


def trigger(events: list[Event]) -> tuple[int, str, int, int] | None:
    """Return the dump's trigger: the last before the dump began (time, reason, thread, value)."""
    found = None
    for e in events:
        if e.id == TRIGGER:
            found = (e.ts, REASONS[e.args[0]] if e.args[0] < len(REASONS) else str(e.args[0]), e.args[1], e.args[2])
        elif e.id == DUMP and e.func == FUNC_START:
            break
    return found


def jobs(events: list[Event]) -> tuple[list[Job], dict[int, int]]:
    """Return the GPU's completed jobs and each channel's submitting thread."""
    out, submitter = [], {}
    for e in events:
        if e.id == GPU_JOB:
            a = e.args[0]
            out.append(Job(e.ts, a & 0xFFFF, a >> 32, bool((a >> 16) & 1), e.args[1], e.args[2]))
        elif e.id == SUBMIT:
            submitter[e.args[0]] = e.tid
    return out, submitter


def _words_to_bytes(words: tuple[int, ...]) -> bytes:
    return b''.join(struct.pack('<Q', w) for w in words)


def vnode_paths(events: list[Event]) -> dict[int, str]:
    """Reconstruct vnode -> path mappings from VFS_LOOKUP / VFS_LOOKUP_DONE multi-event chains."""
    pending: dict[int, tuple[int, bytearray]] = {}
    out: dict[int, str] = {}
    for e in events:
        if e.id not in {VFS_LOOKUP, VFS_LOOKUP_DONE}:
            continue
        if e.func & FUNC_START:
            buf = bytearray(_words_to_bytes(e.args[1:]))
            if e.func & FUNC_END:
                path = bytes(buf).split(b'\x00', 1)[0].decode('utf-8', 'replace')
                if e.args[0] and path:
                    out[e.args[0]] = path
            else:
                pending[e.tid] = (e.args[0], buf)
        elif e.tid in pending:
            vnode, buf = pending[e.tid]
            buf.extend(_words_to_bytes(e.args))
            if e.func & FUNC_END:
                del pending[e.tid]
                path = bytes(buf).split(b'\x00', 1)[0].decode('utf-8', 'replace')
                if vnode and path:
                    out[vnode] = path
    return out


def pageins(events: list[Event]) -> tuple[list[DiskIO], list[PageIn]]:
    """Decode DKRW disk I/O (with vnode paths from VFS_LOOKUP) and FSRW cluster_pagein requests."""
    paths = vnode_paths(events)
    inflight: dict[int, tuple[int, int, int]] = {}  # bp_id: (start_ts, tid, nbytes)
    disk_ios: list[DiskIO] = []
    fs_pageins: list[PageIn] = []
    for e in events:
        if e.id == CL_PAGEIN:
            fs_pageins.append(PageIn(e.ts, e.tid, e.args[1], e.args[0]))
            continue
        if (e.debugid >> 16) != DBG_DKRW_PREFIX:
            continue
        dkcode = (e.debugid >> 2) & 0x3FFF
        bp = e.args[0]
        if dkcode & DKIO_DONE:
            start = inflight.pop(bp, None)
            vnode = e.args[1]
            disk_ios.append(
                DiskIO(
                    ts=e.ts,
                    tid=start[1] if start else e.tid,
                    duration=max(0, e.ts - start[0]) if start else 0,
                    nbytes=start[2] if start else 0,
                    vnode=vnode,
                    path=paths.get(vnode, ''),
                    read=bool(dkcode & DKIO_READ),
                    paging=bool(dkcode & DKIO_PAGING),
                    meta=bool(dkcode & DKIO_META),
                )
            )
        else:
            inflight[bp] = (e.ts, e.tid, e.args[3])
    return disk_ios, fs_pageins


def _syscall_name(debugid: int) -> str | None:
    prefix = debugid >> 16
    num = (debugid >> 2) & 0x3FFF
    if prefix == DBG_BSD_SC_PREFIX:
        return BSD_SYSCALLS.get(num, f'bsd#{num}')
    if prefix == DBG_MACH_SC_PREFIX:
        return MACH_TRAPS.get(num, f'mach#{num}')
    return None


def _track_syscall(e: Event, active_sc: dict[int, tuple[int, str, list[Unnest]]]) -> bool:
    sc = _syscall_name(e.debugid)
    if not sc:
        return False
    if e.func == FUNC_START:
        active_sc[e.tid] = (e.ts, sc, [])
    elif e.func == FUNC_END and (cur := active_sc.pop(e.tid, None)):
        dur = max(0, e.ts - cur[0])
        for u in cur[2]:
            u.syscall_ticks = dur
    return True


def unnests(events: list[Event]) -> tuple[list[Unnest], list[SharedLayout]]:
    """Decode VM_PMAP_UNNEST events (paired with their enclosing syscall) and SHARED_CACHE_LAYOUT spans."""
    active_sc: dict[int, tuple[int, str, list[Unnest]]] = {}
    active_layout: dict[int, int] = {}
    out_unnests: list[Unnest] = []
    out_layouts: list[SharedLayout] = []
    for e in events:
        if _track_syscall(e, active_sc):
            continue
        if e.id == VM_PMAP_UNNEST:
            cur = active_sc.get(e.tid)
            u = Unnest(e.ts, e.tid, e.args[0], e.args[1], cur[1] if cur else '')
            if cur is not None:
                cur[2].append(u)
            out_unnests.append(u)
        elif e.id == SHARED_CACHE_LAYOUT:
            if e.func == FUNC_START:
                active_layout[e.tid] = e.ts
            elif e.func == FUNC_END and (began := active_layout.pop(e.tid, None)) is not None:
                out_layouts.append(SharedLayout(e.ts, e.tid, max(0, e.ts - began), e.args[0]))
    return out_unnests, out_layouts
