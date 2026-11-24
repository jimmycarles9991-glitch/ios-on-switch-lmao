"""The crash, watchdog and stackshot reports pulled with the archive: which belong to this boot, and their stacks.

A report belongs to a boot by its bootSessionUUID; the crash folder keeps every report since the card was made, so a
report of another boot is counted, never read as this one's. Frames print as the device's own symbol when the report
has one, symbolized via `--dsc` / symbol tables when present, or fall back to `dsc 0x<unslid>`, `<image>+0x<offset>`,
and `k 0x<unslid>`.
"""

from __future__ import annotations

import json
import os
import re
from collections import Counter
from dataclasses import dataclass, field

from tools.perf import symbols

KINDS = {
    '308': 'user fault',
    '309': 'crash',
    '409': 'watchdog stackshot',
    '509': 'scene-update watchdog stackshot',
    '288': 'stackshot',
    '298': 'jetsam',
    '145': 'resource (disk writes)',
}
STACK_FRAMES = 12
TOP_CPU = 8
# Per build: the thread_snapshot_wait_flags of that build's XNU (osfmk/kern/block_hint.h) that the device's report
# writer prints as "unknown type N". xnu-10063 (21H461) ends at compressor, 0x14, which its writer names.
WAIT_TYPES = {
    '24B119': {
        21: 'parked bound workqueue',
        22: 'page busy (a page-in or page change in progress)',
        23: 'page list request in progress',
        24: 'pager ready',
        25: 'paging activity',
        26: 'mapping in progress',
        27: 'memory blocked',
        28: 'paging in progress',
        29: 'page-in throttle',
    },
}
UNKNOWN_WAIT = re.compile(r'unknown type (?P<n>\d+)')
BUILD = re.compile(r'\((?P<build>\w+)\)\s*$')


@dataclass
class Report:
    path: str
    bug: str
    name: str
    time: str
    boot: str
    body: dict = field(repr=False)
    build: str = ''

    @property
    def kind(self) -> str:
        return KINDS.get(self.bug, f'bug type {self.bug}')


def load(path: str) -> Report | None:
    with open(path, errors='replace') as f:
        head, _, body = f.read().partition('\n')
    try:
        h, b = json.loads(head), json.loads(body)
    except json.JSONDecodeError:
        return None
    name = h.get('name') or h.get('app_name') or b.get('procName') or os.path.basename(path).split('-')[0]
    build = BUILD.search(h.get('os_version', ''))
    return Report(
        path,
        str(h.get('bug_type')),
        name,
        h.get('timestamp', ''),
        str(b.get('bootSessionUUID', '')).upper(),
        b,
        build['build'] if build else '',
    )


def scan(root: str | None) -> list[Report]:
    out: list[Report] = []
    if not root:
        return out
    for parent, _, files in os.walk(root):
        out += [r for f in sorted(files) if f.endswith('.ips') and (r := load(os.path.join(parent, f)))]
    return out


def spin_files(root: str | None) -> list[str]:
    """Return any tailspin or spindump file in the folder (Apple writes them only when tailspin is enabled)."""
    if not root:
        return []
    names = (f for _, _, files in os.walk(root) for f in files)
    return sorted(f for f in names if f.endswith(('.tailspin', '.spin')) or 'tailspin' in f.lower())


def frame_text(
    image: list | dict | None,
    offset: int,
    images: dict[str, str],
    symbol: str = '',
    syms: symbols.Symbols | None = None,
) -> str:
    if isinstance(image, dict):
        uuid, base, source, name = (
            image.get('uuid', ''),
            image.get('base', 0),
            image.get('source', ''),
            image.get('name'),
        )
    elif isinstance(image, list):
        uuid, base, source, name = image[0], image[1], image[2], None
    else:
        return f'? +0x{offset:x}'
    if symbol:
        return f'{name or "?"} {symbol}'
    if source == 'S':
        return syms.dsc_addr(base + offset) if syms else f'dsc 0x{base + offset:x}'
    if source in {'U', 'K'}:
        return syms.kernel_addr(base + offset) if syms else f'k 0x{base + offset:x}'
    if source == 'A':
        return f'0x{offset:x}'
    label = name or images.get(uuid.upper(), '').rsplit('/', 1)[-1] or uuid[:8]
    return syms.image_addr(label, offset, uuid) if syms else f'{label}+0x{offset:x}'


def crash_lines(r: Report, images: dict[str, str], syms: symbols.Symbols | None = None) -> list[str]:
    b = r.body
    exc, term = b.get('exception', {}), b.get('termination', {})
    out = [f'{exc.get("type", "")} {exc.get("signal", "")} {term.get("indicator", "")}'.strip()]
    threads = b.get('threads') or []
    index = b.get('faultingThread', 0)
    if 0 <= index < len(threads):
        used = b.get('usedImages') or []
        for f in threads[index].get('frames', [])[:STACK_FRAMES]:
            i = f.get('imageIndex', -1)
            out.append(
                '  '
                + frame_text(
                    used[i] if 0 <= i < len(used) else None,
                    f.get('imageOffset', 0),
                    images,
                    f.get('symbol', ''),
                    syms,
                )
            )
    return out


def main_thread(proc: dict) -> tuple[str, dict] | None:
    threads = proc.get('threadById') or {}
    for tid, t in threads.items():
        if 'kThreadMain' in t.get('snapshotFlags', []):
            return tid, t
    return min(threads.items(), key=lambda kv: int(kv[0])) if threads else None


def wait(proc: dict, tid: str, build: str) -> str:
    """Return the stackshot's wait record for one thread, the wait types the device could not name named."""
    names = WAIT_TYPES.get(build, {})
    for w in proc.get('waitInfo') or []:
        if isinstance(w, str) and w.startswith(f'thread {tid}:'):
            return UNKNOWN_WAIT.sub(lambda m: names.get(int(m['n']), m[0]), w)
    return 'no wait record'


def thread_lines(
    proc: dict,
    images_list: list,
    images: dict[str, str],
    build: str,
    syms: symbols.Symbols | None = None,
) -> list[str]:
    found = main_thread(proc)
    if not found:
        return []
    tid, t = found
    out = [
        (
            f'{proc.get("procname")}[{proc.get("pid")}] main thread {tid}: {",".join(t.get("state", []))}, pri '
            f'{t.get("schedPriority")}, user {t.get("userTime", 0):.2f} s, last ran {t.get("lastRunTime", 0):.2f} s '
            f'before; {wait(proc, tid, build)}'
        )
    ]
    for frames in (t.get('kernelFrames') or [])[:4], (t.get('userFrames') or [])[:STACK_FRAMES]:
        for i, offset in frames:
            out.append(
                '  ' + frame_text(images_list[i] if 0 <= i < len(images_list) else None, offset, images, '', syms)
            )
    return out


def stackshot_lines(
    r: Report,
    images: dict[str, str],
    watch: tuple[str, ...],
    syms: symbols.Symbols | None = None,
) -> list[str]:
    ss = r.body.get('stackshot') or r.body
    procs, images_list = ss.get('processByPid') or {}, ss.get('binaryImages') or []
    term = r.body.get('termination', {})
    out = [(term.get('details') or [r.body.get('reason', '')])[0][:200]]
    cpu = sorted(procs.values(), key=lambda p: -(p.get('userTimeTask', 0) + p.get('systemTimeTask', 0)))
    out.append(
        f'CPU since launch at the capture (uptime {r.body.get("uptime")} s): '
        + ', '.join(
            f'{p.get("procname")}[{p.get("pid")}] {p.get("userTimeTask", 0) + p.get("systemTimeTask", 0):.1f} s'
            for p in cpu[:TOP_CPU]
        )
    )
    wanted = {str(r.body.get('pid'))} | {pid for pid, p in procs.items() if p.get('procname') in watch}
    for pid in sorted(wanted, key=lambda x: int(x) if x.isdigit() else 0):
        if pid in procs:
            out += thread_lines(procs[pid], images_list, images, r.build, syms)
    return out


def summary(
    reports: list[Report],
    boots: set[str],
    images: dict[str, str],
    watch: tuple[str, ...],
    syms: symbols.Symbols | None = None,
) -> list[str]:
    mine = [r for r in reports if r.boot in boots]
    others = Counter(r.kind for r in reports if r.boot not in boots)
    out = [
        f'{len(mine)} reports of this boot; {sum(others.values())} of other boots in the folder ('
        + ', '.join(f'{n} {k}' for k, n in others.most_common())
        + ')'
    ]
    for r in mine:
        out.append(f'{r.time[:19]} {r.kind}: {r.name} ({os.path.basename(r.path)})')
        lines = (
            stackshot_lines(r, images, watch, syms) if r.bug in {'409', '509', '288'} else crash_lines(r, images, syms)
        )
        out += ['    ' + line for line in lines]
    return out


def watchdogs(reports: list[Report]) -> list[str]:
    """Return one line per watchdog stackshot in the folder, every boot: when, uptime, the main thread's state."""
    out = []
    for r in sorted((r for r in reports if r.bug in {'409', '509'}), key=lambda r: r.time):
        procs = (r.body.get('stackshot') or {}).get('processByPid') or {}
        proc = procs.get(str(r.body.get('pid')), {})
        found = main_thread(proc)
        state = f'{",".join(found[1].get("state", []))}, {wait(proc, found[0], r.build)}' if found else '-'
        out.append(f'{r.time[:19]} {r.name} at {r.body.get("uptime")} s uptime: main thread {state}')
    return out
