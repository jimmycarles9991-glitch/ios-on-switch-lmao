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
MACH_STACK_HANDOFF = code(1, 0x40, 2)  # a context switch that hands the kernel stack over: thread_invoke's fast path
SWITCHES = {MACH_SCHED, MACH_STACK_HANDOFF}
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
DKIO_PASSIVE = 0x40
DKIO_TIER_MASK, DKIO_TIER_SHIFT = 0xF00, 8
TRACE_NEWTHREAD, TRACE_NEWTHREAD_NAME = code(7, 0, 1), code(7, 1, 1)  # new tid, pid; its process's name
TRACE_EXEC, TRACE_EXEC_NAME = code(7, 0, 2), code(7, 1, 2)  # pid; the name it execs as
TRACE_THREADNAME = code(7, 1, 4)  # a thread's own name, as it sets it
VM_FAULT = code(1, 0x30, 2)  # vm_fault_internal: START, END with its result and the fault's type
FAULT_KINDS = {
    1: 'zero fill',
    2: 'page-in',
    3: 'copy',
    4: 'cache hit',
    7: 'page-in',
    8: 'page-in',
    9: 'compressor',
    10: 'swap-in',
}
PERF_TI_DATA = code(37, 1, 1)
PERF_CS_KDATA, PERF_CS_UDATA = code(37, 2, 3), code(37, 2, 4)
PERF_CS_KHDR, PERF_CS_UHDR = code(37, 2, 5), code(37, 2, 6)
TRIGGER, DUMP, GPU_JOB, GPU_WAIT = code(6, 18, 0x3E0), code(6, 18, 0x3E1), code(6, 18, 0x3E8), code(6, 18, 0x3E9)
SD_PICK = code(6, 18, 0x3D8)  # NativeSwitchBlock's queue picks
GPU_ROOM, GPU_ROOM_DONE = code(6, 18, 0x3EA), code(6, 18, 0x3EB)  # a submission waiting for room in its channel
SD_CLASSES = ('high', 'default', 'low', 'background')
ENGINES = {1: 'GM20B', 2: 'copy engine', 3: 'VIC', 4: 'NVDEC', 5: 'display', 6: 'NVENC', 7: 'promise', 8: 'NVJPG'}
SUBMIT = code(6, 18, 0x3F1)
FRAME_POINTS = {
    code(6, 18, 0x3F0 + i): name
    for i, name in enumerate(
        ('vsync', 'submit', 'gpu done', 'swap', 'present', 'present done', 'flip', 'latch', 'frame ticket')
    )
}
REASONS = ('none', 'late frame', 'hang', 'slow work', 'mark')
# The GPU's stamps are PTIMER units: its 19.2 MHz source with a 32 ns step meant for 31.25 MHz runs at 0.6144 times real
# time (drivers/TegraGM20B/gpu_stats_tick.c).
PTIMER_PER_NS = 0.6144
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
class Submission:
    """One user submission on the GPU: from its first batch's start stamp to its last seqno's end stamp."""

    channel: int
    first: int
    last: int
    start: int  # PTIMER units
    end: int
    ts: int  # when the interrupt pass saw its last seqno done
    tid: int  # the thread that kicked it
    compositor: bool
    bounded: bool = False  # its start stamp was stale: it runs from its channel's previous end, an upper bound

    @property
    def ms(self) -> float:
        return (self.end - self.start) / PTIMER_PER_NS / 1e6


@dataclass(frozen=True)
class GpuWait:
    """A compositor job kicked behind an unsettled dependency (drivers/TegraGM20B/SwitchGpuJobRing.h)."""

    ts: int
    channel: int
    engine: str
    source: int  # the GM20B channel it waits on, 0 for another engine
    value: int


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
    tier: int = 0  # the I/O throttle tier it went out at (0: none)
    passive: bool = False


@dataclass(frozen=True)
class Fault:
    tid: int
    start: int
    end: int
    kind: str


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
    """Return each thread's stretches on a core, (start, end, cpu), from the context switches of either kind."""
    on: dict[int, tuple[int, int]] = {}  # cpu: (thread, since)
    out: dict[int, list[tuple[int, int, int]]] = defaultdict(list)
    first = events[0].ts if events else 0
    for e in events:
        if e.id not in SWITCHES:
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


def submissions(events: list[Event]) -> list[Submission]:
    """Return each channel's submissions as their jobs pair up.

    A submission of several batches has several seqnos: the first batch's job carries the start stamp, and the last
    seqno, the one its SWITCH_KDBG_SUBMIT names, carries the end. A job between them carries neither.
    """
    kicked = {(e.args[0], e.args[1]): e.tid for e in events if e.id == SUBMIT}
    started: dict[int, Job] = {}
    ended: dict[int, int] = {}  # channel: its last submission's end
    out = []
    for j in jobs(events)[0]:
        if j.start_ns:
            started[j.channel] = j
        if (j.channel, j.seqno) not in kicked:
            continue
        first = started.pop(j.channel, None)
        before = ended.get(j.channel, 0)
        if first and j.end_ns >= first.start_ns:
            tid = kicked[(j.channel, j.seqno)]
            # A channel's jobs run in order, each after the one before it ended: a start before that is a stale stamp.
            stale = before and first.start_ns < before <= j.end_ns
            start = before if stale else first.start_ns
            out.append(Submission(j.channel, first.seqno, j.seqno, start, j.end_ns, j.ts, tid, j.compositor, stale))
        if j.end_ns:
            ended[j.channel] = j.end_ns
    return out


@dataclass(frozen=True)
class SdPick:
    ts: int
    lba: int
    sectors: int
    requests: int
    klass: int
    passed: bool
    write: bool
    queued: int
    head_waited_us: int


def sd_picks(events: list[Event]) -> list[SdPick]:
    out = []
    for e in events:
        if e.id == SD_PICK:
            a2, a3 = e.args[1], e.args[2]
            out.append(
                SdPick(
                    e.ts,
                    e.args[0],
                    a2 & 0xFFFFFFFF,
                    a2 >> 32,
                    a3 & 0xFF,
                    bool(a3 & 0x100),
                    bool(a3 & 0x200),
                    a3 >> 16,
                    e.args[3],
                )
            )
    return out


@dataclass(frozen=True)
class RoomWait:
    tid: int
    channel: int
    start: int
    end: int
    get: int
    put: int
    unfinished: int  # the channel's jobs submitted and not done when the wait began
    timed_out: bool


def room_waits(events: list[Event]) -> list[RoomWait]:
    """Return each submission's wait for room in its channel's ring, from its start to its end on the same thread."""
    began: dict[int, Event] = {}
    out = []
    for e in events:
        if e.id == GPU_ROOM:
            began[e.tid] = e
        elif e.id == GPU_ROOM_DONE and (b := began.pop(e.tid, None)) is not None:
            submitted, done = b.args[2] >> 32, b.args[2] & 0xFFFFFFFF
            out.append(
                RoomWait(
                    e.tid,
                    b.args[0],
                    b.ts,
                    e.ts,
                    b.args[1] >> 32,
                    b.args[1] & 0xFFFFFFFF,
                    (submitted - done) & 0xFFFFFFFF,
                    (e.args[2] & 0xFFFFFFFF) != 0,
                )
            )
    return out


def gpu_waits(events: list[Event]) -> list[GpuWait]:
    out = []
    for e in events:
        if e.id == GPU_WAIT:
            a = e.args[0]
            out.append(GpuWait(e.ts, a & 0xFFFF, ENGINES.get((a >> 16) & 0xFFFF, '?'), a >> 32, e.args[1]))
    return out


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
    inflight: dict[int, tuple[int, int, int, int]] = {}  # bp_id: (start_ts, tid, nbytes, the issue's code)
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
            issued = start[3] if start else dkcode
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
                    tier=(issued & DKIO_TIER_MASK) >> DKIO_TIER_SHIFT,
                    passive=bool(issued & DKIO_PASSIVE),
                )
            )
        else:
            inflight[bp] = (e.ts, e.tid, e.args[3], dkcode)
    return disk_ios, fs_pageins


def _strings(events: list[Event], ids: set[int]) -> list[tuple[Event, str]]:
    """Return each string kernel_debug_string_simple wrote (a START...END run per thread) with its first event."""
    pending: dict[tuple[int, int], tuple[Event, bytearray]] = {}
    out = []
    for e in events:
        if e.id not in ids:
            continue
        key = (e.tid, e.id)
        if e.func & FUNC_START:
            pending[key] = (e, bytearray())
        if key not in pending:
            continue
        first, buf = pending[key]
        buf += _words_to_bytes(e.args)
        if e.func & FUNC_END or not e.func:
            del pending[key]
            out.append((first, bytes(buf).split(b'\0', 1)[0].decode('utf-8', 'replace')))
    return out


def thread_names(events: list[Event]) -> dict[int, tuple[int, str, str]]:
    """Return the ring's own names of its threads: tid: (pid, process, thread name).

    They come from the new threads, the execs and the names threads gave themselves while the ring recorded.
    """
    pid_of: dict[int, int] = {}
    proc_of: dict[int, str] = {}
    made_by: dict[int, int] = {}  # creator: the thread it made last
    for e in events:
        if e.id == TRACE_NEWTHREAD:
            pid_of[e.args[0]] = e.args[1]
            made_by[e.tid] = e.args[0]
    exec_pid: dict[int, int] = {e.tid: e.args[0] for e in events if e.id == TRACE_EXEC}
    for first, text in _strings(events, {TRACE_NEWTHREAD_NAME, TRACE_EXEC_NAME}):
        if first.id == TRACE_NEWTHREAD_NAME and first.tid in made_by:
            proc_of.setdefault(made_by[first.tid], text)
        elif first.id == TRACE_EXEC_NAME:
            proc_of[first.tid] = text
            pid_of.setdefault(first.tid, exec_pid.get(first.tid, 0))
    names = {first.tid: text for first, text in _strings(events, {TRACE_THREADNAME})}
    return {
        tid: (pid_of.get(tid, 0), proc_of.get(tid, '?'), names.get(tid, ''))
        for tid in set(pid_of) | set(proc_of) | set(names)
    }


def faults(events: list[Event]) -> list[Fault]:
    """Return each thread's VM faults, their start to their end, and the kind its end names."""
    open_at: dict[int, int] = {}
    out = []
    for e in events:
        if e.id != VM_FAULT:
            continue
        if e.func == FUNC_START:
            open_at[e.tid] = e.ts
        elif e.func == FUNC_END and (began := open_at.pop(e.tid, None)) is not None:
            out.append(Fault(e.tid, began, e.ts, FAULT_KINDS.get(e.args[3] & 0xFF, 'other')))
    return out


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
