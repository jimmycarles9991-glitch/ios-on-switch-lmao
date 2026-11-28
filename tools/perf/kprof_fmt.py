"""SwitchKprof formatting: summaries for headers, IPIs, kernel/user samples, masked chains, and faults."""

from __future__ import annotations

from collections import Counter
from dataclasses import dataclass
from datetime import timedelta

from tools.perf import symbols
from tools.perf.kprof import KprofHeader, KprofSpot, KprofWindow, fmt_kaddr
from tools.perf.windows import Window


@dataclass(frozen=True)
class DetailOptions:
    prefix: str = ''
    focus_proc: str = ''
    include_header: bool = True
    include_user: bool = True
    fn_limit: int = 10
    kt_limit: int = 8
    masked_limit: int = 6
    user_limit: int = 6
    img_limit: int = 4


_DEFAULT_OPTIONS = DetailOptions()


def _fmt_spot(proc: str, spot: KprofSpot, syms: symbols.Symbols | None) -> str:
    if spot.kind == 'dsc':
        return syms.dsc_addr(spot.addr) if syms else f'dsc 0x{spot.addr:x}'
    if spot.kind == 'main+':
        return syms.image_addr(proc, spot.addr) if syms else f'main+0x{spot.addr:x}'
    return 'other'


def _fmt_region(proc: str, reg: KprofSpot, syms: symbols.Symbols | None) -> str:
    if reg.kind == 'dsc':
        return syms.dsc_region(reg.addr) if syms else f'dsc {reg.addr:x}'
    if reg.kind == 'main+':
        return f'{proc}+0x{reg.addr << 20:x}'
    return 'other'


def hot_lines(ws: list[KprofWindow], syms: symbols.Symbols | None, focus_proc: str, limit: int = 4) -> list[str]:
    by_pr: dict[tuple[str, str], Counter[tuple[str, int]]] = {}
    role_totals: Counter[tuple[str, str]] = Counter()
    for kw in ws:
        for u in kw.users:
            key = (u.name, u.role)
            role_totals[key] += u.samples
            bag = by_pr.setdefault(key, Counter())
            for sp in u.spots:
                bag[(sp.kind, sp.addr)] += sp.samples
    ordered = sorted(
        role_totals.items(),
        key=lambda kv: (0 if focus_proc and kv[0][0] == focus_proc else 1, -kv[1], kv[0]),
    )
    out = []
    for (proc, role), total in ordered[:limit]:
        spots = by_pr.get((proc, role), Counter()).most_common(4)
        if spots:
            tag = f' ({role})' if role else ''
            text = ', '.join(f'{_fmt_spot(proc, KprofSpot(k, a, n), syms)} {n}' for (k, a), n in spots)
            out.append(f'kprof user {proc}{tag} ({total} samples): {text}')
    return out


def image_lines(ws: list[KprofWindow], syms: symbols.Symbols | None, focus_proc: str, limit: int = 2) -> list[str]:
    by_pr: dict[tuple[str, str], Counter[tuple[str, int]]] = {}
    totals: Counter[tuple[str, str]] = Counter()
    for kw in ws:
        for im in kw.images:
            key = (im.name, im.role)
            totals[key] += im.samples
            bag = by_pr.setdefault(key, Counter())
            for reg in im.regions:
                bag[(reg.kind, reg.addr)] += reg.samples
    ordered = sorted(
        totals.items(),
        key=lambda kv: (0 if focus_proc and kv[0][0] == focus_proc else 1, -kv[1], kv[0]),
    )
    out = []
    for (proc, role), total in ordered[:limit]:
        regs = by_pr.get((proc, role), Counter()).most_common(4)
        if regs:
            tag = f' ({role})' if role else ''
            text = ', '.join(f'{_fmt_region(proc, KprofSpot(k, a, n), syms)} {n}' for (k, a), n in regs)
            out.append(f'kprof images {proc}{tag} ({total} samples): {text}')
    return out


def _header_mhz_part(hdrs: list[tuple[float, KprofHeader]]) -> str:
    probes = [m for _, h in hdrs for m in h.probe_mhz if m]
    if not probes:
        return ''
    govs = [m for _, h in hdrs for m in h.gov_mhz if m]
    mhz = f'core MHz probe min/p50/max {min(probes)}/{sorted(probes)[len(probes) // 2]}/{max(probes)}'
    return f'{mhz} (gov {min(govs)}..{max(govs)})' if govs else mhz


def _header_line(ws: list[KprofWindow]) -> str:
    hdrs = [(kw.ms / 1000, kw.header) for kw in ws if kw.header]
    if not hdrs:
        return ''
    sec = sum(s for s, _ in hdrs)
    parts = []
    if busy_ws := [h for _, h in hdrs if h.busy_pct]:
        mean_busy = sum(sum(h.busy_pct) / 100 for h in busy_ws) / len(busy_ws)
        mean_irq = sum(sum(h.irq_pct) / 100 for h in busy_ws) / len(busy_ws)
        parts.append(f'busy {mean_busy:.2f} cores (irq {mean_irq:.2f})')
    if sec and any(h.irq_s or h.ipi_s or h.exc_s for _, h in hdrs):
        irq = sum(s * h.irq_s for s, h in hdrs) / sec
        ipi = sum(s * h.ipi_s for s, h in hdrs) / sec
        exc = sum(s * h.exc_s for s, h in hdrs) / sec
        parts.append(f'irq/ipi/exc {irq:.0f}/{ipi:.0f}/{exc:.0f}/s')
    if tot := sum(h.samples for _, h in hdrs):
        u = sum(h.user for _, h in hdrs)
        k = sum(h.kernel for _, h in hdrs)
        kt = sum(h.kernel_task for _, h in hdrs)
        lost = sum(h.lost for _, h in hdrs)
        parts.append(f'samples {tot} (user {u}, kernel {k}, kernel_task {kt}, lost {lost})')
    if mhz := _header_mhz_part(hdrs):
        parts.append(mhz)
    return f'kprof summary ({len(hdrs)} windows): ' + '; '.join(parts) if parts else ''


def _ipi_line(ws: list[KprofWindow]) -> str:
    items = [(kw.ms / 1000, kw.ipis) for kw in ws if kw.ipis]
    if not items or not (sec := sum(s for s, _ in items)):
        return ''
    taken: dict[str, float] = {}
    sched_run: dict[str, float] = {}
    sched_idle: dict[str, float] = {}
    for s, ip in items:
        for k, v in ip.taken.items():
            taken[k] = taken.get(k, 0.0) + s * v
        for ev, (r, i) in ip.sched.items():
            sched_run[ev] = sched_run.get(ev, 0.0) + s * r
            sched_idle[ev] = sched_idle.get(ev, 0.0) + s * i
    t_text = ', '.join(f'{k} {v / sec:.0f}/s' for k, v in sorted(taken.items(), key=lambda kv: -kv[1]) if v)
    s_text = ', '.join(
        f'{ev} {sched_run[ev] / sec:.0f}/{sched_idle[ev] / sec:.0f}/s'
        for ev in sorted(sched_run, key=lambda e: -(sched_run[e] + sched_idle[e]))
        if sched_run[ev] or sched_idle[ev]
    )
    out = f'kprof ipis taken: {t_text}' if t_text else 'kprof ipis'
    return f'{out}; sched run/idle: {s_text}' if s_text else out


def _kernel_fn_line(ws: list[KprofWindow], syms: symbols.Symbols | None, limit: int) -> str:
    fn_s: Counter[tuple[str, int]] = Counter()
    fn_kt: Counter[tuple[str, int]] = Counter()
    fn_caller: dict[tuple[str, int], Counter[tuple[str, int]]] = {}
    fn_owner: dict[tuple[str, int], Counter[str]] = {}
    for kw in ws:
        for f in kw.kernel_fns:
            key = (f.kext, f.addr)
            fn_s[key] += f.samples
            fn_kt[key] += f.kernel_task
            if f.caller_addr:
                fn_caller.setdefault(key, Counter())[(f.caller_kext, f.caller_addr)] += f.samples
            if f.owner:
                fn_owner.setdefault(key, Counter())[f.owner] += f.samples
    if not fn_s:
        return ''
    parts = []
    for (kext, addr), n in fn_s.most_common(limit):
        meta = [f'kt {kt}s'] if (kt := fn_kt[(kext, addr)]) else []
        if caller := fn_caller.get((kext, addr), Counter()).most_common(1):
            meta.append(f'<{fmt_kaddr(*caller[0][0], syms)}')
        if owner := fn_owner.get((kext, addr), Counter()).most_common(1):
            meta.append(f'@{owner[0][0]}')
        parts.append(f'{fmt_kaddr(kext, addr, syms)} {n}s' + (f' ({", ".join(meta)})' if meta else ''))
    return 'kprof kernel: ' + ', '.join(parts)


def _kernel_thread_line(ws: list[KprofWindow], syms: symbols.Symbols | None, limit: int) -> str:
    kt_ws = [kw for kw in ws if kw.kernel_threads]
    if not kt_ws or not (sec := sum(kw.ms for kw in kt_ws) / 1000):
        return ''
    kt_ms: Counter[str] = Counter()
    kt_fn: dict[str, Counter[tuple[str, int]]] = {}
    for kw in kt_ws:
        for t in kw.kernel_threads:
            label = t.name or f'kt:{t.tid}'
            kt_ms[label] += t.ms
            if t.fn_addr:
                kt_fn.setdefault(label, Counter())[(t.fn_kext, t.fn_addr)] += t.ms
    parts = []
    for label, ms in kt_ms.most_common(limit):
        top_fn = kt_fn.get(label, Counter()).most_common(1)
        suffix = f' ({fmt_kaddr(*top_fn[0][0], syms)})' if top_fn else ''
        parts.append(f'{label} {ms / sec:.0f} ms/s{suffix}')
    return 'kprof kernel_task: ' + ', '.join(parts)


def _masked_line(ws: list[KprofWindow], syms: symbols.Symbols | None, limit: int) -> str:
    m_s: Counter[tuple[tuple[str, int], ...]] = Counter()
    m_kt: Counter[tuple[tuple[str, int], ...]] = Counter()
    for kw in ws:
        for m in kw.masked:
            m_s[m.chain] += m.samples
            m_kt[m.chain] += m.kernel_task
    if not m_s:
        return ''
    parts = [
        f'{" < ".join(fmt_kaddr(k, a, syms) for k, a in chain)} {n}s' + (f' (kt {m_kt[chain]}s)' if m_kt[chain] else '')
        for chain, n in m_s.most_common(limit)
    ]
    return 'kprof masked: ' + '; '.join(parts)


def _fault_class_line(ws: list[KprofWindow]) -> str:
    f_ws = [kw for kw in ws if kw.faults]
    if not f_ws or not (sec := sum(kw.ms for kw in f_ws) / 1000):
        return ''
    cls_n: Counter[str] = Counter()
    cls_ms: Counter[str] = Counter()
    af = mod = 0
    for kw in f_ws:
        if kw.faults is None:
            continue
        for cls, (n, ms, _) in kw.faults.classes.items():
            cls_n[cls] += n
            cls_ms[cls] += ms
        af += kw.faults.fast_af
        mod += kw.faults.fast_mod
    total = sum(cls_n.values())
    cls_text = ', '.join(
        f'{cls} {cls_n[cls] / sec:.0f}/s ({cls_ms[cls] / sec:.0f} ms/s)'
        for cls in sorted(cls_n, key=lambda c: (-cls_ms[c], -cls_n[c]))
        if cls_n[cls]
    )
    fast_text = f'; fast {(af + mod) / sec:.0f}/s (af {af / sec:.0f}/s, mod {mod / sec:.0f}/s)' if af or mod else ''
    return f'kprof faults ({total / sec:.0f}/s): {cls_text}{fast_text}'


def _fault_proc_line(ws: list[KprofWindow], limit: int = 6) -> str:
    fp_ws = [kw for kw in ws if kw.fault_procs]
    if not fp_ws or not (sec := sum(kw.ms for kw in fp_ws) / 1000):
        return ''
    p_n: Counter[str] = Counter()
    p_ms: Counter[str] = Counter()
    p_zf: Counter[str] = Counter()
    p_file: Counter[str] = Counter()
    for kw in fp_ws:
        for p in kw.fault_procs:
            p_n[p.name] += p.total
            p_ms[p.name] += p.ms
            p_zf[p.name] += p.zf
            p_file[p.name] += p.file
    parts = [
        f'{name} {n / sec:.0f}/s ({p_ms[name] / sec:.0f} ms/s, zf {p_zf[name] / sec:.0f} file {p_file[name] / sec:.0f})'
        for name, n in p_n.most_common(limit)
    ]
    return 'kprof faults by process: ' + ', '.join(parts) if parts else ''


def detail_lines(
    ws: list[KprofWindow],
    syms: symbols.Symbols | None = None,
    opts: DetailOptions = _DEFAULT_OPTIONS,
) -> list[str]:
    lines: list[str] = []
    if opts.include_header and (hdr := _header_line(ws)):
        lines.append(hdr)
    candidates = (
        _ipi_line(ws),
        _kernel_fn_line(ws, syms, opts.fn_limit),
        _kernel_thread_line(ws, syms, opts.kt_limit),
        _masked_line(ws, syms, opts.masked_limit),
        _fault_class_line(ws),
        _fault_proc_line(ws),
    )
    lines.extend(line for line in candidates if line)
    if opts.include_user:
        lines += hot_lines(ws, syms, opts.focus_proc, limit=opts.user_limit)
        lines += image_lines(ws, syms, opts.focus_proc, limit=opts.img_limit)
    return [opts.prefix + line for line in lines]


def _task_line(ws: list[KprofWindow], prefix: str = '', limit: int = 12) -> str:
    sec = sum(kw.ms for kw in ws) / 1000
    if not sec:
        return ''
    cpu_ms: Counter[str] = Counter()
    samples: Counter[str] = Counter()
    for kw in ws:
        for t in kw.tasks:
            cpu_ms[t.name] += t.cpu_ms
            samples[t.name] += t.user + t.kernel
    if not cpu_ms:
        return ''
    return f'{prefix}kprof tasks: ' + ', '.join(
        f'{n} {ms / sec:.0f} ms/s ({samples[n]}s)' for n, ms in cpu_ms.most_common(limit)
    )


def kprof_summary(ws: list[KprofWindow], syms: symbols.Symbols | None = None) -> list[str]:
    """Summarize all SwitchKprof windows in a boot, symbolizing kernel and user addresses."""
    if not ws:
        return []
    out = [line] if (line := _task_line(ws, prefix='  ', limit=12)) else []
    return out + detail_lines(ws, syms, DetailOptions(prefix='  '))


def kprof_in_window(
    w: Window,
    ws: list[KprofWindow],
    syms: symbols.Symbols | None = None,
    focus_proc: str = '',
) -> list[str]:
    """Summarize SwitchKprof tasks, hot spots, regions, and kernel hogs overlapping one window."""
    chosen = [kw for kw in ws if w.holds(kw.time)]
    if not chosen:
        chosen = [kw for kw in ws if w.overlap(kw.time - timedelta(milliseconds=kw.ms), kw.time)]
    if not chosen:
        return []
    out = [line] if (line := _task_line(chosen, limit=6)) else []
    out += hot_lines(chosen, syms, focus_proc)
    out += image_lines(chosen, syms, focus_proc)
    opts = DetailOptions(
        focus_proc=focus_proc,
        include_header=False,
        include_user=False,
        fn_limit=4,
        kt_limit=4,
        masked_limit=3,
    )
    return out + detail_lines(chosen, syms, opts)
