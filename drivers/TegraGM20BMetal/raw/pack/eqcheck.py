#!/usr/bin/env python3
"""eqcheck (r7-raw): compare two nvdec logs of the same frames, draw by draw.

The logs (capture/cap.cpp RAWCAP_LOG, NVDEC_REGS=1) are one from the shipped m2v + NVK path and one from the raw path.
Submissions without draws or clears (context init, NVK's own setup) are skipped; then the i-th frame of one log is
compared with the i-th of the other.

A draw is the same GPU work when every field matches: the live 3D registers (R line), the render targets (by
first-appearance name), the vertex data, each stage's program and register count, and the bytes each program's constant
buffers hold (pointers replaced by what they point to, handles by their texture/sampler headers). Barriers are compared
as sets: the raw path must cover what NVK emitted before each draw that reads memory written earlier (PSB + TEXINV
before a framebuffer fetch), but for a fetch barrier its harness build marks as left out (ELIDED). A draw the raw path
drew in runs (SPLIT marks) is folded back into one draw first.

usage: eqcheck.py <nvk.log> <raw.log> [--show N] [--flat] [--layouts] [--own]
"""

import collections
import re
import sys
from dataclasses import dataclass, field


def frames(path: str) -> list[list[dict]]:
    subs, cur = [], []
    with open(path) as log:
        for text in log:
            line = text.rstrip('\n')
            if line.startswith('SUBMIT'):
                subs.append(cur)
                cur = []
            elif line.startswith(('DRAW', 'CLEAR', '  ', 'END-OF-SUBMIT')) and not line.startswith(
                ('  STATE', 'P ')
            ):  # DRAW-INDEXED too
                cur.append(line)
    out = []
    for s in subs:
        events, ev = [], None
        for line in s:
            if line.startswith(('DRAW', 'CLEAR')):
                ev = {'head': line, 'lines': []}
                events.append(ev)
            elif line.startswith('  ') and ev is not None:
                ev['lines'].append(line.strip())
        if any(e['head'].startswith('DRAW') for e in events):
            out.append(fold_runs(events))
    # --flat: every submission's events as one stream (a command buffer the raw path submits in segments is several
    # submissions, each of its driver, where m2v makes one)
    if FLAT and out:
        out = [[e for events in out for e in events]]
    return out


DRAW_HEAD = re.compile(r'(DRAW(?:-INDEXED)?) first=(\d+) count=(\d+) begin=(\w+) barriers=(.*)')
# A draw the raw path drew in runs (raw_overlap.h: its own triangles may cover a pixel twice), each run after the first
# marked by its harness build (SWITCH_RAW_HARNESS_MARKS); and what a run's state differs in from the run before: the
# vertex bytes a non-indexed draw reads, up to its own last vertex (the last run's are the whole draw's), and the root
# table's draw parameters, its first vertex (the first run's are the whole draw's)
SPLIT = 'SPLIT'
LAST_RUN = ('vb',)
FIRST_RUN = ('cb0 root draw=',)


def run_state(lines: list[str]) -> list[str]:
    return [x for x in lines if not x.startswith((*LAST_RUN, *FIRST_RUN, 'ib '))]


# An indexed draw's index buffer line (nvdec.cpp), its indices by value
IB = re.compile(r'ib size=(\d+) first=(\d+) count=(\d+) idx=([0-9a-f,]+) restart=(\w+) end=(\w+)$')


def ib_line(lines: list[str]) -> re.Match[str] | None:
    return next((m for m in map(IB.match, lines) if m), None)


def fold_indexed(prev: dict, ev: dict) -> dict | None:
    """Give indexed run `ev` folded into the indexed draw `prev`, or None where it is no run of it.

    A run's indices follow the draw's in the same index buffer; the draw then reads the vertices up to the largest index
    of either (the vertex bytes of the run that reaches further).
    """
    a, b = ib_line(prev['lines']), ib_line(ev['lines'])
    if not a or not b or (a[1], a[5], a[6]) != (b[1], b[5], b[6]) or int(b[2]) != int(a[2]) + int(a[3]):
        return None
    pm, em = DRAW_HEAD.match(prev['head']), DRAW_HEAD.match(ev['head'])
    further = ev if int(em[3]) >= int(pm[3]) else prev
    vertices = {x.split()[0]: x for x in further['lines'] if x.startswith(LAST_RUN)}
    ib = f'ib size={a[1]} first={a[2]} count={int(a[3]) + int(b[3])} idx={a[4]},{b[4]} restart={a[5]} end={a[6]}'
    lines = []
    for line in prev['lines']:
        if line.startswith('ib '):
            lines.append(ib)
        elif line.startswith(LAST_RUN):
            lines.append(vertices.get(line.split()[0], line))
        else:
            lines.append(line)
    count = max(int(pm[3]), int(em[3]))
    return {'head': f'{pm[1]} first={pm[2]} count={count} begin={pm[4]} barriers={pm[5]}', 'lines': lines}


def fold_runs(events: list[dict]) -> list[dict]:
    """Give the events with each split draw's runs folded into one draw of the vertices they cover.

    A run folds into the draw before it when that is a draw of the same kind and state and the run continues it: a
    non-indexed run's vertices begin no later than the draw's end (a list's runs partition its vertices, a strip's share
    the two that join their triangles), an indexed run's indices follow the draw's (fold_indexed). Any other run stays a
    draw of its own, so the comparison fails on it.
    """
    out: list[dict] = []
    for ev in events:
        m = DRAW_HEAD.match(ev['head'])
        barriers = set(m.group(5).split(',')) - {''} if m else set()
        prev = DRAW_HEAD.match(out[-1]['head']) if m and SPLIT in barriers and out else None
        if not prev or prev.group(1) != m.group(1) or prev.group(4) != m.group(4):
            out.append(ev)
            continue
        if run_state(out[-1]['lines']) != run_state(ev['lines']):
            out.append(ev)
            continue
        if m.group(1) == 'DRAW-INDEXED':
            folded = fold_indexed(out[-1], ev)
            if folded is None:
                out.append(ev)
            else:
                out[-1] = folded
            continue
        first, end, start = int(prev.group(2)), int(prev.group(2)) + int(prev.group(3)), int(m.group(2))
        if not first <= start <= end:
            out.append(ev)
            continue
        end = max(end, start + int(m.group(3)))
        last = {x.split()[0]: x for x in ev['lines'] if x.startswith(LAST_RUN)}
        out[-1] = {
            'head': f'{prev.group(1)} first={first} count={end - first} begin={prev.group(4)} barriers={prev.group(5)}',
            'lines': [last.get(x.split()[0], x) if x.startswith(LAST_RUN) else x for x in out[-1]['lines']],
        }
    return out


def rename_all(out: list[list[dict]]) -> list[list[dict]]:
    # persistent resources renamed in order of first appearance in draws (clears of textures no draw uses, such as m2v's
    # initialisation of new textures, would otherwise shift every name)
    names = {}
    for events in out:
        for e in events:
            if e['head'].startswith('DRAW'):
                e['lines'] = [rename(line, names, True) for line in e['lines']]
    for events in out:
        for e in events:
            if e['head'].startswith('CLEAR'):
                e['head'] = rename(e['head'], names, False)
    return out


def rename(text: str, names: dict[str, str], assign: bool) -> str:
    def sub(m: re.Match[str]) -> str:
        if m.group(0) not in names:
            if not assign:
                return 'C' + m.group(0)[1:]
            names[m.group(0)] = f'N{len(names) + 1}'
        return names[m.group(0)]

    return re.sub(r'\bR\d+\b', sub, text)


# --layouts: render targets and sampled images compared by what their texels are, not where or how they lie. The raw
# path renders a target NVK renders through a tiled shadow (rows not 128 apart) through a pitch pool image, and samples
# a surface m2v samples through a shadow image (rows not 32 apart) through a pitch pool image: the texels are the same,
# their headers' addresses, pitches and layouts are not. So: a texture header is its format word, width and height; a
# target its format; a target's address, pitch, memory layout and array registers are dropped, and the depth target's
# address; and the hash of a
# constant buffer that holds handles is not compared (its handles are, normalized; buffer contents keep their own
# hashes).
def layout_tic(m: re.Match[str]) -> str:
    w = [int(x, 16) for x in m.group(2).split()]
    return f'tic[{w[0]:08x} {w[4] & 0xFFFF:04x} {w[5] & 0xFFFF:04x}]'


def normalize_layouts(text: str) -> str:
    text = re.sub(r'tic\[(\S+) ([0-9a-f]{8}(?: [0-9a-f]{8}){7})\]', layout_tic, text)
    if 'handle{' in text:
        text = re.sub(r'(?<=[={])[0-9a-f]{1,16}:', '*:', text)
    return text


LAYOUT_REGS = {'%x' % (0x800 + 0x40 * j + k) for j in range(8) for k in (0x8, 0x14, 0x18, 0x1C)}


def fields(ev: dict) -> collections.OrderedDict:
    f = collections.OrderedDict()
    if LAYOUTS:
        head = re.sub(r'rt=\w+,\d+x\d+,(fmt=\w+),mem=\w+', r'rt=\1', ev['head'])
        ev = {
            'head': re.sub(r'(zs=[0-9a-f]+,[0-9a-f]+),\w+', r'\1', head),
            'lines': [
                re.sub(r'^(rt\d+)=\S+ \S+ (fmt=\w+).*$|^(zt)=\S+$', r'\1\3=\2', normalize_layouts(line))
                for line in ev['lines']
            ],
        }
    h = ev['head']
    if h.startswith('DRAW'):
        m = re.match(r'DRAW(?:-INDEXED)? first=(\d+) count=(\d+) begin=(\w+) barriers=(.*)', h)
        f['draw'] = f'{h.split()[0]} {m.group(1)} {m.group(2)} {m.group(3)}'
        f['barriers'] = {x for x in m.group(4).split(',') if x}
    else:
        m = re.match(r'CLEAR (.*) barriers=(.*)', h)
        f['clear'] = m.group(1)
        f['barriers'] = {x for x in m.group(2).split(',') if x}
    stage = None
    for line in ev['lines']:
        if line.startswith('R '):
            f['R'] = dict(x.split('=') for x in line[2:].split())
            if LAYOUTS:
                f['R'] = {k: v for k, v in f['R'].items() if k not in LAYOUT_REGS}
        elif line.startswith('regs='):
            f['regs'] = line[5:]
        elif line.startswith('stage'):
            stage = line.split()[0]
            f[stage] = line
        elif line.startswith('cb'):
            suffix = ' ' + line.split()[2].split('=')[0] if line.split()[1] == 'root' else ''
            f[f'{stage}.{line.split()[0]}{suffix}'] = line
        else:
            f[line.split('=')[0].split()[0]] = line
    return f


# TIC_POOL_C / TSC_POOL_C: the header pools' last index. The raw path sizes its pools for the render server's live
# textures (8192 texture headers, 1024 samplers; NVK's init: 1024 and 4096): a limit, not draw state. Not compared.
POOL_LIMITS = {'1564', '157c'}

FLAT = '--flat' in sys.argv
LAYOUTS = '--layouts' in sys.argv
# --own: m2v's own draws left out (as --layouts does) with every header and layout still compared: a frame whose m2v
# work draws passes of its own the raw path does without (m2v's copies into an image of every level, its mip generation)
OWN = LAYOUTS or '--own' in sys.argv


def programs(e: dict) -> tuple[str, ...]:
    return tuple(line.split()[1] for line in e['lines'] if line.startswith('stage'))


def drop(frames_of: list[list[dict]], other: list[list[dict]], who: str) -> None:
    theirs = {programs(e) for events in other for e in events if e['head'].startswith('DRAW')}
    dropped = 0
    for events in frames_of:
        keep = [e for e in events if not e['head'].startswith('DRAW') or programs(e) in theirs]
        dropped += len(events) - len(keep)
        events[:] = keep
    if dropped:
        print(f'{dropped} draws of programs only the {who} log runs left out (its own work)')


@dataclass
class Tally:
    show: int
    mism: collections.Counter = field(default_factory=collections.Counter)
    examples: collections.defaultdict = field(default_factory=lambda: collections.defaultdict(list))
    reg_diff: collections.Counter = field(default_factory=collections.Counter)
    bar_missing: collections.Counter = field(default_factory=collections.Counter)
    bar_extra: collections.Counter = field(default_factory=collections.Counter)
    draws: int = 0
    same: int = 0
    clears: int = 0
    elided: int = 0

    def wants(self, kind: str) -> bool:
        return len(self.examples[kind]) < self.show


def compare_frame(tally: Tally, fi: int, fa: list[dict], fb: list[dict]) -> None:
    da = [e for e in fa if e['head'].startswith('DRAW')]
    db = [e for e in fb if e['head'].startswith('DRAW')]
    compare_clears(tally, fi, fa, fb)
    if len(da) != len(db):
        tally.mism['draw count'] += 1
        tally.examples['draw count'].append(f'frame {fi}: nvk {len(da)} raw {len(db)}')
    for di, (ea, eb) in enumerate(zip(da, db, strict=False)):
        tally.draws += 1
        tally.same += compare_draw(tally, fi, di, fields(ea), fields(eb))


def split_depth_stencil(clears: list[str]) -> list[str]:
    """Give each clear's color part, then each one's depth/stencil part on its own, in order.

    NVK clears depth and stencil in the CLEAR_SURFACE of the first color its BeginRendering clears, and m2v decides
    which colors those are (a fetched memoryless color's scratch clears apart): the same texels, packed another way.
    """
    colors, zs = [], []
    for c in clears:
        m = re.match(r'data=([0-9a-f]{8}) (.*?)(?: (zs=\S+))?$', c)
        if not m or not m.group(3):
            colors.append(c)
            continue
        data = int(m.group(1), 16)
        if data & 0x3C:
            colors.append(f'data={data & ~3:08x} {m.group(2)}')
        rect = re.search(r'rect=\S+', c).group(0)
        zs.append(f'{m.group(3)} aspects={data & 3} {rect}')
    return colors + zs


def compare_clears(tally: Tally, fi: int, fa: list[dict], fb: list[dict]) -> None:
    ca = split_depth_stencil([fields(e)['clear'] for e in fa if e['head'].startswith('CLEAR')])
    cb = split_depth_stencil([fields(e)['clear'] for e in fb if e['head'].startswith('CLEAR')])
    if fi == 0:
        extra = [c for c in ca if c not in cb]
        if extra:
            textures = '; '.join(e.split(' color')[0] for e in extra)
            print(f'frame 0: {len(extra)} clears only in nvk (m2v initialises new textures: {textures})')
            ca = [c for c in ca if c in cb]
    if ca != cb:
        tally.mism['clears'] += 1
        if tally.wants('clears'):
            tally.examples['clears'].append(f'frame {fi}\n   nvk {ca}\n   raw {cb}')
    tally.clears += len(ca)


def compare_draw(tally: Tally, fi: int, di: int, x: collections.OrderedDict, y: collections.OrderedDict) -> bool:
    ok = True
    for k in sorted(set(x) | set(y)):
        if k in ('regs', 'barriers'):
            continue
        if k == 'R':
            if not compare_registers(tally, f'frame {fi} draw {di}', x.get('R', {}), y.get('R', {})):
                ok = False
            continue
        if x.get(k) != y.get(k):
            ok = False
            tally.mism[k] += 1
            if tally.wants(k):
                tally.examples[k].append(f'frame {fi} draw {di}\n   nvk {x.get(k)}\n   raw {y.get(k)}')
    if not compare_barriers(tally, f'frame {fi} draw {di}', x['barriers'], y['barriers']):
        ok = False
    return ok


# A fetch barrier the raw path left out because the draw's pixels meet none written since its last one
# (raw_region.h), as its harness builds mark it (SWITCH_RAW_HARNESS_MARKS): expected where NVK has PSB and TEXINV
ELIDED = 'ELIDED'


# A draw that reads a constant buffer over bytes the copy engine wrote after its channel's last constant-cache
# invalidate (nvdec.cpp): wrong on either side
CBSTALE = 'CBSTALE'
# What the raw path must cover of NVK's barriers before a draw. CBINV: NVK invalidates the constant cache as it opens
# each submission and each of m2v's encoders (a primary command buffer of its own), and where a uniform read follows a
# transfer write
REQUIRED = {'TEXINV', 'WFI', 'PSB', 'CBINV'}


def compare_barriers(tally: Tally, where: str, xb: set[str], yb: set[str]) -> bool:
    missing = xb - yb
    if ELIDED in yb and missing <= {'PSB', 'TEXINV'}:
        tally.elided += 1
        missing = set()
    for m in missing:
        tally.bar_missing[m] += 1
    for m in yb - xb - {ELIDED}:
        tally.bar_extra[m] += 1
    if CBSTALE in xb | yb:
        tally.mism['constant cache stale'] += 1
        if tally.wants('constant cache stale'):
            tally.examples['constant cache stale'].append(f'{where}: nvk {sorted(xb)} raw {sorted(yb)}')
        return False
    if not missing & REQUIRED:
        return True
    tally.mism['barrier (fetch/pass)'] += 1
    if tally.wants('barrier (fetch/pass)'):
        tally.examples['barrier (fetch/pass)'].append(f'{where}: nvk {sorted(xb)} raw {sorted(yb)}')
    return False


def compare_registers(tally: Tally, where: str, ra: dict[str, str], rb: dict[str, str]) -> bool:
    d = sorted(m for m in set(ra) | set(rb) if ra.get(m) != rb.get(m) and m not in POOL_LIMITS)
    if not d:
        return True
    tally.mism['registers'] += 1
    for m in d:
        tally.reg_diff[m] += 1
    if tally.wants('registers'):
        regs = ' '.join(f'{m} nvk={ra.get(m, "-")} raw={rb.get(m, "-")}' for m in d[:12])
        tally.examples['registers'].append(f'{where}: {regs}')
    return False


def report(tally: Tally) -> int:
    print(
        f'draws compared {tally.draws}, identical {tally.same}; clears compared {tally.clears}; '
        f'fetch barriers raw left out {tally.elided}'
    )
    for k, n in tally.mism.most_common():
        print(f'MISMATCH {k:<28} {n}')
        for e in tally.examples[k]:
            print('  ' + e)
    if tally.reg_diff:
        print('registers differing (draws):', ' '.join(f'{m}:{n}' for m, n in tally.reg_diff.most_common(40)))
    if tally.bar_missing:
        print('barriers nvk had, raw not (draws):', dict(tally.bar_missing))
    if tally.bar_extra:
        print('barriers raw had, nvk not (draws):', dict(tally.bar_extra))
    return 0 if tally.draws and tally.same == tally.draws and not tally.mism else 1


def main() -> int:
    show = int(sys.argv[sys.argv.index('--show') + 1]) if '--show' in sys.argv else 3
    a, b = frames(sys.argv[1]), frames(sys.argv[2])
    print(f'frames: nvk {len(a)} raw {len(b)}')
    if OWN:  # (before the names: what is left out names nothing)
        # each driver's own draws: programs no draw of the other log runs (m2v's shadow copies and mip work; the raw
        # path's 4:2:0 conversions through the pack's ycbcr states, where m2v converts with its own)
        theirs_b = [list(events) for events in b]
        drop(a, theirs_b, 'nvk')
        drop(b, a, 'raw')
    a, b = rename_all(a), rename_all(b)
    tally = Tally(show)
    for fi, (fa, fb) in enumerate(zip(a, b, strict=False)):
        compare_frame(tally, fi, fa, fb)
    return report(tally)


if __name__ == '__main__':
    sys.exit(main())
