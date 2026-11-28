"""Frame pacing: present intervals per window, and every slow present with its timeline and the stage that lost it.

The display's 5 s stats line carries every present's interval as a histogram (a still screen's windows are not
logged); a slow present (two periods or more) has its own SwitchFrame line, joined here with the render thread's
SwitchFrameRender line for the same vsync and the GPU user side's SwitchFrameUser lines committed before its swap.

The stage that lost the period is the first point of the chain past the frame end the present was due at: the render
thread's wake, its first GPU submission (recording), its swap (encoding), the GPU's completion, the flip. The longest
stage is not that stage: a frame that missed waits most of a period for the next scanout.

The display's first and last submit are the compositor's first and last since the previous swap, so a frame whose
work began before its vsync shows a first submit before its origin, up to hundreds of milliseconds after a still
screen. That says the recording did not start late; it does not say when this frame's recording began. The render
thread's own first submission after the vsync does, from its tail on the SwitchFrame line or its SwitchFrameRender
line.

A present's periods are split at its origin: those from the frame end it was due at to its latch were lost in its own
chain, and the rest of its interval passed before its vsync, with no frame on its way.
"""

import bisect
import itertools
import math
import re
import statistics
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
RENDER_FIRST = re.compile(r'first submit (?P<first>\d+) us')
# The render thread's window on the SwitchFrame line (debug kernels from 26 Nov): its vsync, wake and first submission.
FRAME_RENDER = re.compile(r'; render vsync (?P<vsync>\d+) woke (?P<woke>-?\d+) first (?P<first>-?\d+)')
SPLIT = re.compile(
    r'swap (?P<swap>\d+) us \(cpu (?P<cpu>\d+), blocked (?P<blocked>\d+) us in \d+, waiting for a core (?P<runq>\d+) us'
)
LONGEST = re.compile(r'longest block (?P<us>\d+) us, user pc (?P<pc>.+?) lr (?P<lr>.+?), kernel')
NOT_LOST = {'idle', 'rate', 'masked'}
GAP = 'gap before it'
WAITED = 'render thread waited'
SWAP_AGREES_US = 2000
TICKS_PER_MS = int(TICK_HZ) // 1000
PERIODS_MS = (100, 125, 200, 250, 500, 1000, 2000, 4000, 5000, 10000, 15000, 30000, 60000)
PHASE_BINS = 10
BURST_GAP_MS = 50
CYCLE_MIN_CYCLES = 3
CYCLE_P_MAX = 0.005
CORRELATE_WINDOW_MS = 250
CORRELATE_P_MAX = 0.01
PERIODIC_FAMILIES = (
    ('SwitchPower', 'SwitchPower: '),
    ('SwitchSched', 'SwitchSched 1/2'),
    ('SwitchKprof', 'SwitchKprof: '),
    ('SwitchDisplay stats', 'SwitchDisplay stats: '),
    ('tegra-gm20b stats', 'tegra-gm20b: stats '),
)


@dataclass
class Stats:
    end: datetime
    ms: int
    hist: tuple[int, int, int, int]
    max_ms: float


@dataclass
class Render:
    woke_us: int
    first_us: int = -1
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
    window: tuple[int, int] | None = None  # the render thread's (wake, first submit) from the SwitchFrame line, us

    @property
    def woke_us(self) -> int | None:
        if self.window:
            return self.window[0]
        return self.render.woke_us if self.render else None

    @property
    def first_us(self) -> int | None:
        """Return when this frame's recording reached the GPU, in us after its vsync; None when no line says."""
        if self.window and self.window[1] >= 0:
            return self.window[1]
        if self.render and self.render.first_us >= 0:
            return self.render.first_us
        first = self.frame.points['first']
        return first if self.frame.submits and first >= 0 else None

    @property
    def chain_periods(self) -> int:
        """Return the periods lost from the frame end it was due at to its latch."""
        f = self.frame
        if f.origin_kind != 'vsync' or f.points['at_latch'] < 0:
            return f.frames - 1
        return min(f.frames - 1, max(0, round(f.points['at_latch'] / PERIOD_US) - 1))


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
    if m := RENDER_FIRST.search(rest):
        out.first_us = int(m['first'])
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
    server, not recording; which one needs the commit probe. A frame with no recording point (no compositor
    submission, or one before its vsync with no render line) goes from its wake to its swap.
    """
    f, r = slow.frame, slow.render
    if f.cause in NOT_LOST:
        return f'not lost ({f.cause})'
    if f.origin_kind != 'vsync':
        return 'unanchored'
    p = f.points
    if 0 <= p['at_latch'] <= PERIOD_US * 3 // 2:
        return GAP
    if (woke := slow.woke_us) is not None and woke > PERIOD_US:
        return 'late wake'
    if p['swap'] < 0:
        return 'unclassified'
    first = slow.first_us
    chain = (
        ('record', first if first is not None else -1),
        ('encode', p['swap']),
        ('display queue', p['offer']),
        ('gpu', p['done']),
        ('flip', p['flip']),
    )
    for stage, at in chain:
        if at > PERIOD_US:
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
            out.append(Slow(r.time, parse_frame(m), window=render_window(r.msg, int(m['origin']))))
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


def render_window(msg: str, origin: int) -> tuple[int, int] | None:
    """Return the render thread's (wake, first submit) on a SwitchFrame line, in us after the present's vsync.

    None when the line has none or the render thread's window began at another vsync than the present's origin.
    """
    m = FRAME_RENDER.search(msg)
    if not m or abs(int(m['vsync']) - origin) > JOIN_TICKS:
        return None
    return int(m['woke']), int(m['first'])


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
    before = f.frames - 1 - s.chain_periods
    out = [
        f'{s.time:%H:%M:%S.%f}'[:-3]
        + f' {f.frames} periods ({f.interval_us / 1000:.1f} ms), kernel cause {f.cause}, lost in: {s.stage}'
        + (f' ({s.chain_periods} in its chain, {before} before its vsync)' if before else ''),
        f'    from {f.origin_kind}: ' + ', '.join(marks) + f', latch {p["at_latch"] / 1000:+.1f} ms',
    ]
    if s.window:
        first = f'{s.window[1] / 1000:+.1f} ms' if s.window[1] >= 0 else 'none'
        out.append(f'    render thread window: woke {s.window[0] / 1000:+.1f} ms, first submit {first}')
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


@dataclass(frozen=True)
class Correlation:
    family: str
    hits: int
    episodes: int
    raw_hits: int
    raw_total: int
    events: int
    expected: float
    median_delta_ms: float
    p_value: float


def _is_hitch(s: Slow) -> bool:
    return counted(s) or s.stage in {GAP, WAITED}


def _phase_window_ms(period_ms: int) -> float:
    return min(period_ms / PHASE_BINS, max(500.0, period_ms / 40))


def _binomial_tail(n: int, k: int, p: float) -> float:
    if k <= 0:
        return 1.0
    if k > n or p <= 0.0:
        return 0.0
    if p >= 1.0:
        return 1.0
    log_p, log_q = math.log(p), math.log1p(-p)
    lg_n = math.lgamma(n + 1)
    total = 0.0
    for i in range(k, n + 1):
        total += math.exp(lg_n - math.lgamma(i + 1) - math.lgamma(n - i + 1) + i * log_p + (n - i) * log_q)
    return min(1.0, total)


def _collapse_ticks(ticks: list[int], gap_ticks: int) -> list[int]:
    out: list[int] = []
    prev = -1
    for t in ticks:
        if not out or t - prev > gap_ticks:
            out.append(t)
        prev = t
    return out


def _phase_cluster(episodes: list[int], span: int, win: int) -> tuple[int, int]:
    ep_phases = sorted((t % span, t) for t in episodes)
    ring = ep_phases + [(p + span, t) for p, t in ep_phases]
    best_cycles, best_start = 0, 0
    j = 0
    n_ep = len(episodes)
    for i in range(n_ep):
        start = ring[i][0]
        while j < i + n_ep and ring[j][0] - start <= win:
            j += 1
        # Subtracting the circular window's start keeps a wrap-around burst in one cycle index.
        cycles = len({(t - start) // span for _, t in ring[i:j]})
        if cycles > best_cycles:
            best_cycles, best_start = cycles, start
    return best_cycles, best_start


def _fit_period(ticks: list[int], episodes: list[int], period_ms: int) -> tuple[int, int, int, int, int] | None:
    span = period_ms * TICKS_PER_MS
    win_ms = round(_phase_window_ms(period_ms))
    win = round(_phase_window_ms(period_ms) * TICKS_PER_MS)
    cycles, start = _phase_cluster(episodes, span, win)
    if cycles < CYCLE_MIN_CYCLES or cycles * 10 < len(episodes) * 3:
        return None
    raw = sum((t - start) % span <= win for t in ticks)
    if raw * 10 < len(ticks) * 3:
        return None
    if len(episodes) * _binomial_tail(len(episodes) - 1, cycles - 1, win / span) > CYCLE_P_MAX:
        return None
    start_ms = start // TICKS_PER_MS
    end_ms = start_ms + win_ms if start_ms + win_ms <= period_ms else start_ms + win_ms - period_ms
    return period_ms, raw, cycles, start_ms, end_ms


def _format_period(period_ms: int) -> str:
    return f'{period_ms // 1000} s' if period_ms >= 2000 else f'{period_ms} ms'


def periodic(slows: list[Slow]) -> str:
    """Return the longest cycle the lost presents keep to (a timer or a poll behind them), or ''.

    Tests sub-second and multi-second periods with a circular phase window and a distinct-cycle scan bound so isolated
    bursts do not masquerade as multi-second cycles. Presents on a 500 ms grid are also on the 250, 125 and 100 ms
    grids; the longest cycle that holds nearly as many cycles as the best is the one named.
    """
    ticks = sorted(s.frame.latch for s in slows if _is_hitch(s))
    episodes = _collapse_ticks(ticks, BURST_GAP_MS * TICKS_PER_MS)
    if len(episodes) < CYCLE_MIN_CYCLES:
        return ''
    found = [fit for period_ms in PERIODS_MS if (fit := _fit_period(ticks, episodes, period_ms))]
    if not found:
        return ''
    best = max(found, key=lambda f: f[2])[2]
    period_ms, raw, cycles, start_ms, end_ms = max(f for f in found if f[2] * 10 >= best * 9)
    return (
        f'{raw} of {len(ticks)} late presents keep to a {_format_period(period_ms)} cycle '
        f'(phase {start_ms}-{end_ms} ms, {cycles} cycles)'
    )


def _collapse_times(times: list[datetime], gap: timedelta) -> list[datetime]:
    out: list[datetime] = []
    prev: datetime | None = None
    for t in times:
        if prev is None or t - prev > gap:
            out.append(t)
        prev = t
    return out


def _nearest_event(events: list[datetime], t: datetime, window: timedelta) -> tuple[int, float] | None:
    i = bisect.bisect_left(events, t)
    candidates = [idx for idx in (i - 1, i) if 0 <= idx < len(events) and abs(t - events[idx]) <= window]
    if not candidates:
        return None
    best = min(candidates, key=lambda idx: abs(t - events[idx]))
    return best, (t - events[best]).total_seconds() * 1000


def _count_near(events: list[datetime], times: list[datetime], window: timedelta) -> int:
    return sum(_nearest_event(events, t, window) is not None for t in times)


def _merged_coverage_s(events: list[datetime], lo: datetime, hi: datetime, window: timedelta) -> float:
    intervals = [(a, b) for e in events if (a := max(lo, e - window)) < (b := min(hi, e + window))]
    if not intervals:
        return 0.0
    total = 0.0
    cur_a, cur_b = intervals[0]
    for a, b in intervals[1:]:
        if a > cur_b:
            total += (cur_b - cur_a).total_seconds()
            cur_a, cur_b = a, b
        elif b > cur_b:
            cur_b = b
    return total + (cur_b - cur_a).total_seconds()


def _covered_fraction(events: list[datetime], lo: datetime, hi: datetime, window: timedelta) -> float:
    span_s = (hi - lo).total_seconds()
    if span_s <= 0:
        return 1.0
    # Active-only lines (SwitchDisplay stats) go quiet when the screen is still; median gap floors the local rate.
    gaps = [(b - a).total_seconds() for a, b in itertools.pairwise(events) if b > a]
    local_p = 2 * window.total_seconds() / statistics.median(gaps) if gaps else 1.0
    return min(1.0, max(_merged_coverage_s(events, lo, hi, window) / span_s, local_p))


def _periodic_event_times(recs: list[Rec]) -> dict[str, list[datetime]]:
    by_family: dict[str, list[datetime]] = {name: [] for name, _ in PERIODIC_FAMILIES}
    for r in recs:
        if r.proc != 'kernel':
            continue
        for name, prefix in PERIODIC_FAMILIES:
            if r.msg.startswith(prefix):
                by_family[name].append(r.time)
                break
    return {name: sorted(times) for name, times in by_family.items()}


def _correlate_family(
    family: str,
    events: list[datetime],
    raw_times: list[datetime],
    episodes: list[datetime],
    window: timedelta,
) -> Correlation | None:
    if len(events) < CYCLE_MIN_CYCLES:
        return None
    matched = [m for t in episodes if (m := _nearest_event(events, t, window))]
    distinct = len({idx for idx, _ in matched})
    if distinct < CYCLE_MIN_CYCLES:
        return None
    lo, hi = min(events[0], episodes[0]) - window, max(events[-1], episodes[-1]) + window
    p = _covered_fraction(events, lo, hi, window)
    expected = len(episodes) * p
    p_value = _binomial_tail(len(episodes), len(matched), p)
    if len(matched) <= expected or p_value > CORRELATE_P_MAX:
        return None
    deltas = [delta for _, delta in matched]
    return Correlation(
        family=family,
        hits=len(matched),
        episodes=len(episodes),
        raw_hits=_count_near(events, raw_times, window),
        raw_total=len(raw_times),
        events=distinct,
        expected=expected,
        median_delta_ms=statistics.median(deltas),
        p_value=p_value,
    )


def correlate_periodic(slows: list[Slow], recs: list[Rec], window_ms: int = CORRELATE_WINDOW_MS) -> list[Correlation]:
    """Correlate late presents against nearby periodic kernel log lines, rejecting chance coincidences."""
    raw_times = sorted(s.time for s in slows if _is_hitch(s))
    episodes = _collapse_times(raw_times, timedelta(milliseconds=BURST_GAP_MS))
    if len(episodes) < CYCLE_MIN_CYCLES:
        return []
    window = timedelta(milliseconds=window_ms)
    by_family = _periodic_event_times(recs)
    out = [
        corr
        for name, _ in PERIODIC_FAMILIES
        if (corr := _correlate_family(name, by_family[name], raw_times, episodes, window))
    ]
    return sorted(out, key=lambda c: (c.p_value, -c.hits))


def periodic_correlations(slows: list[Slow], recs: list[Rec], window_ms: int = CORRELATE_WINDOW_MS) -> list[str]:
    """Format significant correlations between late presents and periodic kernel log lines."""
    return [
        f'{c.raw_hits} of {c.raw_total} late presents ({c.hits}/{c.episodes} episodes, expected {c.expected:.1f}) '
        f'land within {window_ms} ms of {c.family} ({c.events} lines, median {c.median_delta_ms:+.0f} ms)'
        for c in correlate_periodic(slows, recs, window_ms)
    ]
