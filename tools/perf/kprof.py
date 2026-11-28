"""SwitchKprof parsing and reporting: per-window CPU headers, IPIs, kernel/user samples, masked chains, and faults."""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from datetime import datetime, timedelta

from tools.perf import symbols
from tools.perf.source import Rec

KPROF_KEEP = (
    'SwitchKprof ipis',
    'SwitchKprof tasks',
    'SwitchKprof kernel',
    'SwitchKprof masked',
    'SwitchKprof user',
    'SwitchKprof waits',
    'SwitchKprof images',
    'SwitchKprof faults',
)
KPROF_HEAD = re.compile(r'^SwitchKprof: (?P<ms>\d+) ms, (?P<body>.*)$')
KPROF_SAMPLES = re.compile(
    r'samples (?P<total>\d+) \(\d+ ms each\): user (?P<user>\d+), kernel (?P<kernel>\d+) '
    r'\(kernel_task (?P<kt>\d+)\), lost (?P<lost>\d+)'
)
KPROF_IPIS = re.compile(
    r'^SwitchKprof ipis per s, taken \((?P<kinds>[^)]+)\):(?P<taken>[^;]*)'
    r'(?:;\s*scheduler sent to running/idle CPUs:(?P<sched>.*))?$'
)
KPROF_SCHED_IPI = re.compile(r'(?P<ev>[^\s/]+)\s+(?P<run>\d+)/(?P<idle>\d+)')
KPROF_TASK = re.compile(r'(?P<name>[^,;]+?)\[(?P<pid>-?\d+)\] (?P<user>\d+)\+(?P<kernel>\d+)=(?P<ms>\d+)ms')
KPROF_KERN_ITEM = re.compile(r'^(?P<fn>\S+)\s+(?P<samples>\d+)\s+(?P<kt>\d+)\s+<(?P<caller>\S+)\s+@(?P<owner>.+)$')
KPROF_KT_ITEM = re.compile(r"^(?P<tid>[0-9a-fA-F]+)\s+'(?P<name>[^']*)'\s+(?P<ms>\d+)(?:\s+(?P<fn>\S+))?$")
KPROF_MASK_ITEM = re.compile(r'^(?P<samples>\d+)\s+(?P<kt>\d+)\s+(?P<chain>.+)$')
KPROF_FAULT_HEAD = re.compile(r'^SwitchKprof faults: (?P<rate>\d+)/s \([^)]*\):(?P<classes>[^;]+);(?P<rest>.*)$')
KPROF_FAULT_CLS = re.compile(
    r'(?P<cls>zf|cow|soft|device|file|cmp|failed)\s+(?P<n>\d+)\s+(?P<ms>\d+)\s+(?P<max_us>\d+)'
)
KPROF_FAULT_FAST = re.compile(r'fast \d+/s \(af (?P<af>\d+), mod (?P<mod>\d+)\)')
KPROF_FAULT_PROC = re.compile(
    r'(?P<name>[^;]+?)\[(?P<pid>-?\d+)\]\s+(?P<n>\d+):\s+(?P<zf>\d+)\s+(?P<cow>\d+)\s+(?P<soft>\d+)\s+'
    r'(?P<dev>\d+)\s+(?P<file>\d+)\s+(?P<cmp>\d+)\s+(?P<fail>\d+),\s+(?P<ms>\d+) ms;\s+'
    r'\d+,\s+\d+,\s+\d+;\s+(?P<af>\d+)/(?P<mod>\d+)'
)
KPROF_USER_PREFIX = re.compile(r'^SwitchKprof user \d+/\d+:\s*')
KPROF_USER_ROLE = re.compile(
    r'^(?P<name>[^\[\]]+?)\[(?P<pid>-?\d+)\] (?P<total>\d+) '
    r'\(main (?P<main>\d+), worker (?P<worker>\d+)\):(?P<rest>.*)$'
)
KPROF_USER_PLAIN = re.compile(r'^(?P<name>[^\[\]]+?)\[(?P<pid>-?\d+)\] (?P<total>\d+):(?P<body>.*)$')
KPROF_ROLE_SEG = re.compile(r'(?P<role>main|worker):(?P<body>[^;]*)')
KPROF_SPOT = re.compile(r'(?P<kind>dsc|main\+)\s+(?P<addr>[0-9a-fA-F]+)\s+(?P<n>\d+)')
KPROF_OTHER = re.compile(r'other\s+(?P<n>\d+)')
KPROF_IMAGES = re.compile(
    r'^SwitchKprof images: (?P<name>.+?)\[(?P<pid>-?\d+)\] '
    r'(?:(?P<role>main|worker) (?P<rsamp>\d+)/)?(?P<total>\d+):(?P<body>.*?); lost (?P<lost>\d+)'
)
KPROF_IMG_ITEM = re.compile(r'(?P<kind>dsc|main\+|other)\s+(?P<mb>[0-9a-fA-F]+)\s+(?P<n>\d+)')
KPROF_GROUP_GAP = timedelta(seconds=2)


@dataclass(frozen=True)
class KprofHeader:
    busy_pct: tuple[int, ...] = ()
    irq_pct: tuple[float, ...] = ()
    irq_s: int = 0
    ipi_s: int = 0
    exc_s: int = 0
    samples: int = 0
    user: int = 0
    kernel: int = 0
    kernel_task: int = 0
    lost: int = 0
    probe_mhz: tuple[int, ...] = ()
    gov_mhz: tuple[int, ...] = ()


@dataclass(frozen=True)
class KprofIpis:
    taken: dict[str, int]
    sched: dict[str, tuple[int, int]]


@dataclass(frozen=True)
class KprofKernelFn:
    kext: str
    addr: int
    samples: int
    kernel_task: int
    caller_kext: str
    caller_addr: int
    owner: str


@dataclass(frozen=True)
class KprofKernelThread:
    tid: str
    name: str
    ms: int
    fn_kext: str
    fn_addr: int


@dataclass(frozen=True)
class KprofMasked:
    samples: int
    kernel_task: int
    chain: tuple[tuple[str, int], ...]


@dataclass(frozen=True)
class KprofFaults:
    classes: dict[str, tuple[int, int, int]]
    fast_af: int = 0
    fast_mod: int = 0


@dataclass(frozen=True)
class KprofProcFaults:
    name: str
    pid: int
    total: int
    zf: int
    cow: int
    soft: int
    file: int
    cmp: int
    ms: int
    fast_af: int
    fast_mod: int


@dataclass(frozen=True)
class KprofTask:
    name: str
    pid: int
    user: int
    kernel: int
    cpu_ms: int


@dataclass(frozen=True)
class KprofSpot:
    kind: str  # 'dsc' or 'main+'
    addr: int
    samples: int


@dataclass(frozen=True)
class KprofUser:
    name: str
    pid: int
    role: str  # 'main', 'worker', or '' (legacy unsplit line)
    samples: int
    spots: tuple[KprofSpot, ...]
    other: int


@dataclass(frozen=True)
class KprofImage:
    name: str
    pid: int
    role: str
    samples: int
    regions: tuple[KprofSpot, ...]


@dataclass
class KprofWindow:
    time: datetime
    ms: int = 10000
    header: KprofHeader | None = None
    ipis: KprofIpis | None = None
    tasks: list[KprofTask] = field(default_factory=list)
    kernel_fns: list[KprofKernelFn] = field(default_factory=list)
    kernel_threads: list[KprofKernelThread] = field(default_factory=list)
    masked: list[KprofMasked] = field(default_factory=list)
    faults: KprofFaults | None = None
    fault_procs: list[KprofProcFaults] = field(default_factory=list)
    users: list[KprofUser] = field(default_factory=list)
    images: list[KprofImage] = field(default_factory=list)


def kprof(recs: list[Rec]) -> tuple[str, list[Rec]]:
    """Return kprof's state in this boot (off, absent, on, or the boot window alone) and its per-window lines."""
    lines = [r for r in recs if r.proc == 'kernel' and r.msg.startswith('SwitchKprof')]
    if not lines:
        return 'absent', []
    if any(r.msg.startswith('SwitchKprof: off') for r in lines):
        return 'off (built without --//kernel:kprof)', []
    kept = [r for r in lines if KPROF_HEAD.match(r.msg) or r.msg.startswith(KPROF_KEEP)]
    if over := next((r for r in lines if r.msg.startswith('SwitchKprof: boot window over')), None):
        return f'the boot window only, until {over.time:%H:%M:%S} (a --//kernel:recorder build)', kept
    return 'on', kept


def parse_kaddr(tok: str) -> tuple[str, int]:
    if not tok or tok == '?':
        return '', 0
    kext, hx = tok.rsplit('@', 1) if '@' in tok else ('', tok)
    try:
        return kext, int(hx, 16)
    except ValueError:
        return kext, 0


def fmt_kaddr(kext: str, addr: int, syms: symbols.Symbols | None) -> str:
    if addr == 0:
        return '?'
    if syms and (hit := syms.kernel.lookup(addr)):
        return hit
    if kext:
        return f'{kext}@0x{addr:x}'
    return syms.kernel_addr(addr) if syms else f'k 0x{addr:x}'


def _parse_header(body: str) -> KprofHeader:
    busy_pct: list[int] = []
    irq_pct: list[float] = []
    irq_s = ipi_s = exc_s = total = user = kernel = kt = lost = 0
    probe_mhz: list[int] = []
    gov_mhz: list[int] = []
    for seg in body.split(';'):
        s = seg.strip()
        if 'busy/irq %:' in s:
            for b, i in re.findall(r'(\d+)/([\d.]+)', s.split(':', 1)[1]):
                busy_pct.append(int(b))
                irq_pct.append(float(i))
        elif s.startswith('irq/ipi/exc per s:'):
            for i, p, e in re.findall(r'(\d+)/(\d+)/(\d+)', s.split(':', 1)[1]):
                irq_s, ipi_s, exc_s = irq_s + int(i), ipi_s + int(p), exc_s + int(e)
        elif sm := KPROF_SAMPLES.search(s):
            total, user, kernel, kt, lost = (int(sm[k]) for k in ('total', 'user', 'kernel', 'kt', 'lost'))
        elif s.startswith('core MHz (probe):'):
            probe_mhz = [int(x) for x in re.findall(r'\d+', s.split(':', 1)[1])]
        elif s.startswith('governor MHz:'):
            gov_mhz = [int(x) for x in re.findall(r'\d+', s.split(':', 1)[1])]
    return KprofHeader(
        tuple(busy_pct),
        tuple(irq_pct),
        irq_s,
        ipi_s,
        exc_s,
        total,
        user,
        kernel,
        kt,
        lost,
        tuple(probe_mhz),
        tuple(gov_mhz),
    )


def _parse_ipis(msg: str) -> KprofIpis | None:
    m = KPROF_IPIS.match(msg)
    if not m:
        return None
    kinds = [k.strip() for k in m['kinds'].split('/') if k.strip()]
    taken: dict[str, int] = {}
    for cpu_part in m['taken'].split(','):
        nums = [int(x) for x in cpu_part.strip().split('/') if x.isdigit()]
        for k, v in zip(kinds, nums, strict=False):
            taken[k] = taken.get(k, 0) + v
    sched = {sm['ev']: (int(sm['run']), int(sm['idle'])) for sm in KPROF_SCHED_IPI.finditer(m['sched'] or '')}
    return KprofIpis(taken, sched) if taken or sched else None


def _parse_kernel(msg: str) -> list[KprofKernelFn]:
    if '):' not in msg:
        return []
    out = []
    for seg in msg.split('):', 1)[1].split(';'):
        if m := KPROF_KERN_ITEM.match(seg.strip()):
            fk, fa = parse_kaddr(m['fn'])
            ck, ca = parse_kaddr(m['caller'])
            if fa:
                out.append(KprofKernelFn(fk, fa, int(m['samples']), int(m['kt']), ck, ca, m['owner'].strip()))
    return out


def _parse_kt(msg: str) -> list[KprofKernelThread]:
    marker = 'top sampled fn:'
    if marker not in msg:
        return []
    out = []
    for seg in msg.split(marker, 1)[1].split(';'):
        if m := KPROF_KT_ITEM.match(seg.strip()):
            fk, fa = parse_kaddr(m['fn'] or '')
            out.append(KprofKernelThread(m['tid'], m['name'], int(m['ms']), fk, fa))
    return out


def _parse_masked(msg: str) -> list[KprofMasked]:
    if '):' not in msg:
        return []
    out = []
    for seg in msg.split('):', 1)[1].split(';'):
        if m := KPROF_MASK_ITEM.match(seg.strip()):
            parsed = (parse_kaddr(tok.strip()) for tok in m['chain'].split('<') if tok.strip())
            chain = tuple((k, a) for k, a in parsed if a)
            if chain:
                out.append(KprofMasked(int(m['samples']), int(m['kt']), chain))
    return out


def _parse_faults(msg: str) -> KprofFaults | None:
    m = KPROF_FAULT_HEAD.match(msg)
    if not m:
        return None
    classes = {
        cm['cls']: (int(cm['n']), int(cm['ms']), int(cm['max_us'])) for cm in KPROF_FAULT_CLS.finditer(m['classes'])
    }
    fm = KPROF_FAULT_FAST.search(m['rest'])
    return KprofFaults(classes, int(fm['af']) if fm else 0, int(fm['mod']) if fm else 0)


def _parse_fault_procs(msg: str) -> list[KprofProcFaults]:
    if '):' not in msg:
        return []
    return [
        KprofProcFaults(
            m['name'].strip(),
            int(m['pid']),
            int(m['n']),
            int(m['zf']),
            int(m['cow']),
            int(m['soft']),
            int(m['file']),
            int(m['cmp']),
            int(m['ms']),
            int(m['af']),
            int(m['mod']),
        )
        for m in KPROF_FAULT_PROC.finditer(msg.split('):', 1)[1])
    ]


def _parse_spots(body: str) -> tuple[tuple[KprofSpot, ...], int]:
    spots = tuple(KprofSpot(m['kind'], int(m['addr'], 16), int(m['n'])) for m in KPROF_SPOT.finditer(body))
    other = int(m['n']) if (m := KPROF_OTHER.search(body)) else 0
    return spots, other


def _parse_user(msg: str) -> list[KprofUser]:
    rest = KPROF_USER_PREFIX.sub('', msg, count=1).strip()
    if m := KPROF_USER_ROLE.match(rest):
        name, pid = m['name'].strip(), int(m['pid'])
        counts = {'main': int(m['main']), 'worker': int(m['worker'])}
        return [
            KprofUser(name, pid, seg['role'], counts[seg['role']], *_parse_spots(seg['body']))
            for seg in KPROF_ROLE_SEG.finditer(m['rest'])
        ]
    return [
        KprofUser(pm['name'].strip(), int(pm['pid']), '', int(pm['total']), *_parse_spots(pm['body']))
        for chunk in rest.split(';')
        if (pm := KPROF_USER_PLAIN.match(chunk.strip()))
    ]


def _parse_images(msg: str) -> KprofImage | None:
    m = KPROF_IMAGES.match(msg)
    if not m:
        return None
    regions = tuple(KprofSpot(it['kind'], int(it['mb'], 16), int(it['n'])) for it in KPROF_IMG_ITEM.finditer(m['body']))
    samples = int(m['rsamp']) if m['rsamp'] else int(m['total'])
    return KprofImage(m['name'].strip(), int(m['pid']), m['role'] or '', samples, regions)


def _ingest_tasks(w: KprofWindow, msg: str) -> None:
    body = msg.split(':', 1)[1] if ':' in msg else ''
    w.tasks += [
        KprofTask(tm['name'].strip(), int(tm['pid']), int(tm['user']), int(tm['kernel']), int(tm['ms']))
        for tm in KPROF_TASK.finditer(body)
    ]


def _ingest_ipis(w: KprofWindow, msg: str) -> None:
    if parsed := _parse_ipis(msg):
        w.ipis = parsed


def _ingest_faults(w: KprofWindow, msg: str) -> None:
    if pf := _parse_faults(msg):
        w.faults = pf


def _ingest_images(w: KprofWindow, msg: str) -> None:
    if img := _parse_images(msg):
        w.images.append(img)


_INGESTERS = (
    ('SwitchKprof ipis ', _ingest_ipis),
    ('SwitchKprof tasks', _ingest_tasks),
    ('SwitchKprof kernel ', lambda w, m: w.kernel_fns.extend(_parse_kernel(m))),
    ('SwitchKprof kernel_task:', lambda w, m: w.kernel_threads.extend(_parse_kt(m))),
    ('SwitchKprof masked', lambda w, m: w.masked.extend(_parse_masked(m))),
    ('SwitchKprof faults:', _ingest_faults),
    ('SwitchKprof faults by process ', lambda w, m: w.fault_procs.extend(_parse_fault_procs(m))),
    ('SwitchKprof user ', lambda w, m: w.users.extend(_parse_user(m))),
    ('SwitchKprof images:', _ingest_images),
)


def _has_data(w: KprofWindow) -> bool:
    return bool(
        w.tasks
        or w.users
        or w.images
        or w.kernel_fns
        or w.kernel_threads
        or w.masked
        or w.ipis
        or w.faults
        or w.fault_procs
        or (w.header and w.header.samples)
    )


def kprof_windows(recs: list[Rec]) -> list[KprofWindow]:
    """Group SwitchKprof lines into one KprofWindow per 10 s summary burst."""
    out: list[KprofWindow] = []
    for r in recs:
        if r.proc != 'kernel' or not r.msg.startswith('SwitchKprof'):
            continue
        if m := KPROF_HEAD.match(r.msg):
            out.append(KprofWindow(r.time, int(m['ms']), header=_parse_header(m['body'])))
            continue
        for prefix, fn in _INGESTERS:
            if r.msg.startswith(prefix):
                if not out or abs(r.time - out[-1].time) > KPROF_GROUP_GAP:
                    out.append(KprofWindow(r.time))
                fn(out[-1], r.msg)
                break
    return [w for w in out if _has_data(w)]
