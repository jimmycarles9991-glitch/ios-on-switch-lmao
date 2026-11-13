"""Where each slow frame's time went, from a debug boot's pulled log archive.

Reads NativeLegacyDisplay's "SwitchFrame:" lines (one per present that came 1.5 refresh periods or more after the one
before), the kernel's "SwitchFrameRender:" lines (Core Animation's display thread from the vsync to its swap: on a
core, blocked, waiting for one) and, when the GPU user side logs them, "SwitchFrameUser:" lines, then prints every slow
frame's stages and a summary of which stage cost the most.
"""

import argparse
import re
import subprocess
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field

TICK_NS = 1e9 / 19.2e6  # the Tegra X1 generic timer: mach absolute time's unit on the Switch

FRAME = re.compile(
    r'SwitchFrame: latch (?P<latch>\d+) after (?P<frames>\d+) frames \((?P<us>\d+) us\), cause (?P<cause>\w+|\?); '
    r'origin (?P<origin_kind>\w+) (?P<origin>\d+); first submit (?P<first>-?\d+), last submit (?P<last>-?\d+) '
    r'\((?P<submits>\d+), exec (?P<exec>\d+) us\), swap (?P<swap>-?\d+), offer (?P<offer>-?\d+), '
    r'gpu done (?P<done>-?\d+), flip (?P<flip>-?\d+), latch (?P<at_latch>-?\d+) us; other submits (?P<others>\d+); '
    r'swap thread base (?P<base>-?\d+) sched (?P<sched>-?\d+) mode (?P<mode>-?\d+) qos (?P<qos>-?\d+); '
    r'cpu (?P<khz_vsync>\d+) MHz at vsync (?P<khz_swap>\d+) at swap'
    r'(?:; imp (?P<imp>\d+) (?P<imp_us>\d+) us, bind (?P<bind>\d+) (?P<bind_us>\d+) us, '
    r'alloc (?P<alloc>\d+) (?P<alloc_us>\d+) us, free (?P<free>\d+), '
    r'clean (?P<clean_kib>\d+) KiB (?P<clean_us>\d+) us)?'
)
USER = re.compile(r'SwitchFrameUser: (?P<proc>\S+) cb (?P<cb>\d+) (?P<rest>.*)')
RENDER = re.compile(r'SwitchFrameRender: vsync (?P<vsync>\d+): (?P<rest>.*)$')
INTERACTION = re.compile(
    r'^(?P<time>\S+ \S+).*SwitchFrames interaction: (?P<ms>\d+) ms \(touch (?P<touch>\d+) ms\), (?P<presents>\d+) '
    r'presents: 1:(?P<f1>\d+) 2:(?P<f2>\d+) 3:(?P<f3>\d+) 4\+:(?P<f4>\d+), (?P<lost>\d+) periods lost, '
    r'worst (?P<worst>[\d.]+) ms, still (?P<still>\d+); long (?P<causes>[a-z0-9 ]+?)(?:; (?P<rest>.*))?$'
)
LATENCY = re.compile(r'SwitchFrames latency: (?P<rest>.*)$')
USER_FIELD = re.compile(r'(\w+) (\d+)(?: (\d+))?')

# The kernel's stages, between consecutive points of a frame (microseconds from its origin; -1: not seen).
STAGES = [
    ('vsync->first submit', 'origin', 'first', "the compositor's CPU work before its first GPU submission"),
    ('first->last submit', 'first', 'last', 'its CPU work between GPU submissions'),
    ('last submit->swap', 'last', 'swap', 'encoding after the last submission up to the swap'),
    ('swap->offer', 'swap', 'offer', "the swap waiting in the display's queue"),
    ('offer->gpu done', 'offer', 'done', "the GPU finishing the compositor's work and the present"),
    ('gpu done->flip', 'done', 'flip', 'the completion interrupt to the flip'),
    ('flip->latch', 'flip', 'at_latch', 'the flip waiting for the next scanout boundary'),
]
# The kernel's new-surface work in the frame (every process): counts and microseconds.
SURFACE_FIELDS = ('imp', 'imp_us', 'bind', 'bind_us', 'alloc', 'alloc_us', 'free', 'clean_kib', 'clean_us')
MODES = {1: 'realtime', 2: 'fixed', 3: 'timeshare'}
QOS = {0: 'unspecified', 1: 'maintenance', 2: 'background', 3: 'utility', 4: 'default', 5: 'user-initiated', 6: 'UI'}


@dataclass
class Frame:
    """One slow present, its points in microseconds from its origin."""

    latch: int
    frames: int
    interval_us: int
    cause: str
    origin_kind: str
    origin: int
    points: dict
    submits: int
    exec_us: int
    others: int
    thread: tuple
    khz: tuple
    surfaces: dict = field(default_factory=dict)
    user: list = field(default_factory=list)
    render: str = ''

    def stages(self) -> list[tuple[str, int, str]]:
        """Return (name, microseconds, meaning) for each stage whose two ends were seen."""
        out = []
        for name, start, end, meaning in STAGES:
            a = 0 if start == 'origin' else self.points[start]
            b = self.points[end]
            if a >= 0 and b >= 0 and b >= a:
                out.append((name, b - a, meaning))
        return out


def parse_frame(m: re.Match) -> Frame:
    """Build a Frame from a SwitchFrame line's match."""
    g = m.groupdict()
    points = {k: int(g[k]) for k in ('first', 'last', 'swap', 'offer', 'done', 'flip', 'at_latch')}
    return Frame(
        latch=int(g['latch']),
        frames=int(g['frames']),
        interval_us=int(g['us']),
        cause=g['cause'],
        origin_kind=g['origin_kind'],
        origin=int(g['origin']),
        points=points,
        submits=int(g['submits']),
        exec_us=int(g['exec']),
        others=int(g['others']),
        thread=(int(g['base']), int(g['sched']), int(g['mode']), int(g['qos'])),
        khz=(int(g['khz_vsync']), int(g['khz_swap'])),
        surfaces={k: int(g[k]) for k in SURFACE_FIELDS if g[k] is not None},
    )


def parse_user(m: re.Match) -> tuple[str, int, dict[str, tuple[int, int]]]:
    """Return (process, command buffer, {field: (start ticks, end ticks)}) from a SwitchFrameUser line."""
    spans = {}
    for name, a, b in USER_FIELD.findall(m.group('rest')):
        spans[name] = (int(a), int(b) if b else int(a))
    return m.group('proc'), int(m.group('cb')), spans


def read_lines(path: str) -> list[str]:
    """Return the log's lines: `log show` of a .logarchive, or a text file as is."""
    if path.endswith(('.logarchive', '.logarchive/')):
        out = subprocess.run(
            [
                '/usr/bin/log',
                'show',
                '--archive',
                path,
                '--style',
                'compact',
                '--predicate',
                'eventMessage BEGINSWITH "SwitchFrame"',
            ],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        return out.splitlines()
    with open(path, errors='replace') as f:
        return f.read().splitlines()


def collect(lines: list[str]) -> list[Frame]:
    """Return the slow frames, each with the user lines of the commits that preceded its swap."""
    frames, users, renders = [], defaultdict(list), {}
    for line in lines:
        m = FRAME.search(line)
        if m:
            frames.append(parse_frame(m))
            continue
        m = RENDER.search(line)
        if m:
            renders[int(m.group('vsync'))] = m.group('rest')
            continue
        m = USER.search(line)
        if m:
            proc, cb, spans = parse_user(m)
            commit = spans.get('commit', (0, 0))[0]
            if commit:
                users[proc].append((commit, cb, spans))
    for entries in users.values():
        entries.sort()
    for frame in frames:
        join(frame, users, renders)
    return frames


def join(frame: Frame, users: dict, renders: dict) -> None:
    """Give the frame its render thread's line (same vsync) and each process's last commit before its swap."""
    if frame.origin_kind == 'vsync':
        frame.render = renders.get(frame.origin, '')
    if frame.points['swap'] < 0:
        return
    swap_ticks = frame.origin + frame.points['swap'] * 1000 / TICK_NS
    for proc, entries in users.items():
        before = [e for e in entries if e[0] <= swap_ticks]
        if before:
            frame.user.append((proc, *before[-1]))


def interactions(lines: list[str]) -> list[str]:
    """Return one row per touch interaction window and one per latency line, which follows its window."""
    out = []
    for line in lines:
        m = LATENCY.search(line)
        if m:
            out.append(f'    latency: {m.group("rest")}')
            continue
        m = INTERACTION.search(line)
        if not m:
            continue
        g = m.groupdict()
        intervals = max(0, int(g['presents']) - 1)
        locked = 100 * int(g['f1']) / intervals if intervals else 0
        out.append(
            f'{g["time"][11:23]} {g["ms"]:>5} ms (touch {g["touch"]:>5}): {g["presents"]:>4} presents, '
            f'1:{g["f1"]} 2:{g["f2"]} 3:{g["f3"]} 4+:{g["f4"]} ({locked:.0f}% one frame apart), worst {g["worst"]} ms, '
            f'still {g["still"]}; long {g["causes"]}'
        )
        if g['rest']:
            out.append(f'    {g["rest"]}')
    return out


def describe(frame: Frame) -> list[str]:
    """Return the lines printed for one slow frame."""
    out = [
        (
            f'latch {frame.latch}: {frame.frames} frames ({frame.interval_us / 1000:.1f} ms), cause {frame.cause}, '
            f'from {frame.origin_kind}'
        )
    ]
    worst = max(frame.stages(), key=lambda s: s[1], default=None)
    for name, us, meaning in frame.stages():
        mark = '  <-' if worst and name == worst[0] else ''
        out.append(f'  {name:22} {us / 1000:7.2f} ms  {meaning}{mark}')
    out.append(
        f'  compositor submits {frame.submits} (kernel exec {frame.exec_us} us), '
        f"other processes' submits {frame.others}"
    )
    if frame.render:
        out.append(f'  render thread: {frame.render}')
    base, sched, mode, qos = frame.thread
    out.append(
        f'  swap thread base {base} sched {sched} {MODES.get(mode, mode)} QoS {QOS.get(qos, qos)}; '
        f'CPU {frame.khz[0]} MHz at vsync, {frame.khz[1]} at swap'
    )
    n = frame.surfaces
    if n:
        out.append(
            f'  new surfaces: {n["imp"]} imports ({n["imp_us"]} us, '
            f'cleaned {n["clean_kib"]} KiB in {n["clean_us"]} us), '
            f'{n["bind"]} binds ({n["bind_us"]} us), {n["alloc"]} allocs ({n["alloc_us"]} us), {n["free"]} frees'
        )
    for proc, _commit, cb, spans in frame.user:
        parts = []
        for name, (a, b) in spans.items():
            if not a:
                continue
            start = (a - frame.origin) * TICK_NS / 1e6
            parts.append(f'{name} {start:+.2f}' + (f'..{(b - frame.origin) * TICK_NS / 1e6:+.2f}' if b != a else ''))
        out.append(f'  {proc} cb {cb}: ' + ', '.join(parts) + ' ms')
    return out


def summary(frames: list[Frame]) -> list[str]:
    """Return the counts by cause and by the costliest stage, and each stage's mean."""
    causes = Counter(f.cause for f in frames)
    worst = Counter()
    totals, counts = defaultdict(float), Counter()
    for f in frames:
        stages = f.stages()
        if stages:
            worst[max(stages, key=lambda s: s[1])[0]] += 1
        for name, us, _ in stages:
            totals[name] += us
            counts[name] += 1
    lost = sum(f.frames - 1 for f in frames)
    out = [f'{len(frames)} slow presents, {lost} refresh periods lost']
    out.append('  by cause: ' + ', '.join(f'{c} {n}' for c, n in causes.most_common()))
    out.append('  costliest stage: ' + ', '.join(f'{s} {n}' for s, n in worst.most_common()))
    for name, *_ in STAGES:
        if counts[name]:
            out.append(f'  {name:22} mean {totals[name] / counts[name] / 1000:7.2f} ms over {counts[name]}')
    return out


def main(argv: list[str] | None = None) -> int:
    """Print every slow frame of the log, then the summary."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('log', help='a pulled .logarchive, or a text file of the log')
    parser.add_argument('--summary', action='store_true', help='only the summary')
    args = parser.parse_args(argv)
    lines = read_lines(args.log)
    frames = collect(lines)
    if not frames:
        print('no SwitchFrame lines: a debug kernel logs them; check the archive covers the boot')
        return 1
    if not args.summary:
        for frame in frames:
            print('\n'.join(describe(frame)))
    rows = interactions(lines)
    if rows:
        print('touch interactions:')
        print('\n'.join(rows))
    print('\n'.join(summary(frames)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
