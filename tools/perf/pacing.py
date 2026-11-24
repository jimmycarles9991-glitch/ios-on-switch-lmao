"""Frame pacing: present intervals per window, and every slow present with its timeline and the stage that lost it.

The display's 5 s stats line carries every present's interval as a histogram (a still screen's windows are not
logged); a slow present (two periods or more) has its own SwitchFrame line, joined here with the render thread's
SwitchFrameRender line for the same vsync and the GPU user side's SwitchFrameUser lines committed before its swap.

The stage that lost the period is the first point of the chain past the frame end the present was due at: the render
thread's wake, its first GPU submission (recording), its swap (encoding), the GPU's completion, the flip. The longest
stage is not that stage: a frame that missed waits most of a period for the next scanout.
"""

import bisect
import re
from collections import Counter
from dataclasses import dataclass, field
from datetime import datetime, timedelta

from tools.frames.frames import FRAME, RENDER, USER, Frame, parse_frame, parse_user
from tools.perf.source import TICK_HZ, Rec
from tools.perf.windows import Window

PERIOD_US = 16667
JOIN_TICKS = int(TICK_HZ / 1000)
STATS = re.compile(
    r'SwitchDisplay stats: (?P<ms>\d+) ms: .*?present (?P<presents>\d+) .*?max (?P<max>[\d.]+) ms, frames '
    r'1:(?P<f1>\d+) 2:(?P<f2>\d+) 3:(?P<f3>\d+) 4\+:(?P<f4>\d+)'
)
WOKE = re.compile(r'woke (?P<woke>\d+) us after it')
SPLIT = re.compile(
    r'swap (?P<swap>\d+) us \(cpu (?P<cpu>\d+), blocked (?P<blocked>\d+) us in \d+, waiting for a core (?P<runq>\d+) us'
)
LONGEST = re.compile(r'longest block (?P<us>\d+) us, user pc (?P<pc>.+?) lr (?P<lr>.+?), kernel')
NOT_LOST = {'idle', 'rate', 'masked'}
GAP = 'gap before it'
WAITED = 'render thread waited'
SWAP_AGREES_US = 2000
PERIODS_MS = (100, 125, 200, 250, 500, 1000)
PHASE_BINS = 10


@dataclass
class Stats:
    end: datetime
    ms: int
    hist: tuple[int, int, int, int]
    max_ms: float


@dataclass
class Render:
    woke_us: int
    swap_us: int = -1
    cpu_us: int = 0
    blocked_us: int = 0
    runq_us: int = 0
    longest_us: int = 0
    longest: str = ''


@dataclass
class Slow:
    time: datetime
    frame: Frame
    render: Render | None = None
    users: list[tuple[str, int, dict]] = field(default_factory=list)  # (process, cb, spans) committed before the swap
    stage: str = ''


def stats_windows(recs: list[Rec]) -> list[Stats]:
    out = []
    for r in recs:
        if r.proc == 'kernel' and r.msg.startswith('SwitchDisplay stats') and (m := STATS.search(r.msg)):
            hist = (int(m['f1']), int(m['f2']), int(m['f3']), int(m['f4']))
            out.append(Stats(r.time, int(m['ms']), hist, float(m['max'])))
    return out


def distribution(window: Window, stats: list[Stats]) -> tuple[list[int], float, float, bool]:
    """Return the present-interval histogram (1, 2, 3, 4+ periods) of the stats windows mostly inside the window.

    Also the ms they cover, the longest interval among them, and whether a window shorter than one stats window took
    the stats windows around it instead.
    """
    overlaps = [(s, window.overlap(s.end - timedelta(milliseconds=s.ms), s.end)) for s in stats]
    chosen = [s for s, inside in overlaps if inside * 2 >= s.ms]
    around = not chosen
    if around:
        chosen = [s for s, inside in overlaps if inside > 0]
    hist = [sum(s.hist[i] for s in chosen) for i in range(4)]
    return hist, sum(s.ms for s in chosen), max((s.max_ms for s in chosen), default=0.0), around


def parse_render(rest: str) -> Render | None:
    woke = WOKE.search(rest)
    if not woke:
        return None
    out = Render(int(woke['woke']))
    if m := SPLIT.search(rest):
        out.swap_us, out.cpu_us, out.blocked_us, out.runq_us = (int(m[k]) for k in ('swap', 'cpu', 'blocked', 'runq'))
    if m := LONGEST.search(rest):
        out.longest_us = int(m['us'])
        out.longest = f'{m["pc"]}, lr {m["lr"]}'
    return out


def split(render: Render | None) -> str:
    """Name the render thread's largest part of its swap: on a core, blocked, or waiting for one."""
    if render is None or render.swap_us < 0:
        return ''
    parts = {'on core': render.cpu_us, 'blocked': render.blocked_us, 'no core': render.runq_us}
    return max(parts, key=parts.__getitem__)


def lost_stage(slow: Slow) -> str:  # noqa: PLR0911  one return per stage of the chain, in its order
    """Name the first point of the chain past the frame end the present was due at (see the module docstring).

    A present that latched one period after its origin kept its own chain: the frame before it never presented (it
    came late and was replaced, its content came late, or there was none; these lines cannot tell which). A render
    thread that spent half a period or more in one block before its swap was waiting, for the app's commit or for a
    server, not recording; which one needs the commit probe.
    """
    f, r = slow.frame, slow.render
    if f.cause in NOT_LOST:
        return f'not lost ({f.cause})'
    if f.origin_kind != 'vsync':
        return 'unanchored'
    p = f.points
    if 0 <= p['at_latch'] <= PERIOD_US * 3 // 2:
        return GAP
    if r is not None and r.woke_us > PERIOD_US:
        return 'late wake'
    if p['first'] < 0 or p['swap'] < 0:
        return 'unclassified'
    stages = (('first', 'record'), ('swap', 'encode'), ('offer', 'display queue'), ('done', 'gpu'), ('flip', 'flip'))
    for point, stage in stages:
        if p[point] > PERIOD_US:
            if stage in {'record', 'encode'} and r and r.longest_us * 2 >= PERIOD_US:
                return WAITED
            return f'{stage}, {split(r)}' if stage in {'record', 'encode'} and r else stage
    return 'latch' if p['flip'] >= 0 else 'unclassified'


def slow_presents(recs: list[Rec]) -> list[Slow]:
    renders: dict[int, Render] = {}
    users: list[tuple[int, str, int, dict]] = []
    out = []
    for r in recs:
        if r.msg.startswith('SwitchFrameUser') and (m := USER.search(r.msg)):
            proc, cb, spans = parse_user(m)
            if spans.get('commit', (0, 0))[0]:
                users.append((spans['commit'][0], proc, cb, spans))
        elif r.proc != 'kernel':
            continue
        elif r.msg.startswith('SwitchFrameRender') and (m := RENDER.search(r.msg)):
            if render := parse_render(m['rest']):
                renders[int(m['vsync'])] = render
        elif r.msg.startswith('SwitchFrame: ') and (m := FRAME.search(r.msg)):
            out.append(Slow(r.time, parse_frame(m)))
    users.sort(key=lambda u: u[0])
    vsyncs = sorted(renders)
    commits = [u[0] for u in users]
    for s in out:
        if s.frame.origin_kind == 'vsync' and (v := nearest(vsyncs, s.frame.origin)) is not None:
            s.render = renders[v] if same_swap(renders[v], s.frame) else None
        lo, hi = commit_span(s.frame)
        s.users = [
            (proc, cb, spans)
            for _, proc, cb, spans in users[bisect.bisect_left(commits, lo) : bisect.bisect_right(commits, hi)]
        ]
        s.stage = lost_stage(s)
    return out


def nearest(vsyncs: list[int], tick: int) -> int | None:
    """Return the render line's vsync within JOIN_TICKS of the frame's.

    The render thread's stand-in vsync is stamped on the display's work loop, tens of microseconds after the frame
    end the SwitchFrame line names.
    """
    i = bisect.bisect_left(vsyncs, tick)
    near = [v for v in vsyncs[max(0, i - 1) : i + 1] if abs(v - tick) <= JOIN_TICKS]
    return min(near, key=lambda v: abs(v - tick)) if near else None


def same_swap(r: Render, f: Frame) -> bool:
    """Whether a render line describes this present: the swap it timed is the one the display took."""
    return r.swap_us < 0 or f.points['swap'] < 0 or abs(r.swap_us - f.points['swap']) <= SWAP_AGREES_US


def commit_span(f: Frame) -> tuple[float, float]:
    """Return the ticks in which a commit belongs to the present: from a period before its origin to its swap."""
    if f.points['swap'] < 0:
        return 1.0, 0.0
    before = PERIOD_US if f.origin_kind == 'vsync' else f.interval_us
    return f.origin - before * TICK_HZ / 1e6, f.origin + f.points['swap'] * TICK_HZ / 1e6


def timeline(s: Slow) -> list[str]:
    """Return the lines printed for one slow present: its chain, its render thread and the commits before it."""
    f, p = s.frame, s.frame.points
    marks = [f'{k} {p[k] / 1000:+.1f}' for k in ('first', 'last', 'swap', 'offer', 'done', 'flip') if p[k] >= 0]
    out = [
        f'{s.time:%H:%M:%S.%f}'[:-3]
        + f' {f.frames} periods ({f.interval_us / 1000:.1f} ms), kernel cause {f.cause}, lost in: {s.stage}',
        f'    from {f.origin_kind}: ' + ', '.join(marks) + f', latch {p["at_latch"] / 1000:+.1f} ms',
    ]
    if r := s.render:
        out.append(
            f'    render thread: woke {r.woke_us} us after the vsync; swap {r.swap_us} us = on core {r.cpu_us} + '
            f'blocked {r.blocked_us} + no core {r.runq_us}; longest block {r.longest_us} us at {r.longest or "-"}'
        )
    for proc, cb, spans in s.users[-4:]:
        commit = spans['commit'][0]
        rel = (commit - f.origin) * 1000 / TICK_HZ
        parts = [f'commit {rel:+.1f} ms']
        for name in ('enc', 'fence', 'done'):
            if spans.get(name, (0, 0))[0]:
                a, b = spans[name]
                parts.append(f'{name} {(a - f.origin) * 1000 / TICK_HZ:+.1f}..{(b - f.origin) * 1000 / TICK_HZ:+.1f}')
        out.append(f'    {proc} cb {cb}: ' + ', '.join(parts))
    return out


UNTOLD = {'unanchored', GAP, WAITED}


def counted(s: Slow) -> bool:
    """Whether a slow present lost a period in its own chain: not cadence, not a stage these lines cannot place."""
    return not s.stage.startswith('not lost') and s.stage not in UNTOLD


def periodic(slows: list[Slow]) -> str:
    """Return the longest cycle the lost presents keep to (a timer or a poll behind them), or ''.

    Presents on a 500 ms grid are also on the 250, 125 and 100 ms grids; the longest cycle that holds nearly as many
    of them as the best is the one named.
    """
    ticks = [s.frame.latch for s in slows if counted(s) or s.stage in {GAP, WAITED}]
    if len(ticks) < 20:
        return ''
    found = []
    for period_ms in PERIODS_MS:
        span = period_ms * TICK_HZ / 1000
        top, n = Counter(int((t % span) / span * PHASE_BINS) for t in ticks).most_common(1)[0]
        if n / len(ticks) >= 0.3:
            found.append((period_ms, n, top))
    if not found:
        return ''
    best = max(n for _, n, _ in found)
    period_ms, n, top = max(f for f in found if f[1] * 10 >= best * 9)
    return (
        f'{n} of {len(ticks)} late presents keep to a {period_ms} ms cycle '
        f'(phase {top * period_ms // PHASE_BINS}-{(top + 1) * period_ms // PHASE_BINS} ms)'
    )
