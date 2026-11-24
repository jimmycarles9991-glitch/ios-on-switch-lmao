"""First-use work and Apple's own stall marks, each as what it cost the thread that waited.

Ours: state creations, compiles and imports. Apple's: Core Animation's pipeline creations, and the UIKit update
cycles, Core Animation transactions and render-server frame updates Apple itself marks as stalls.
"""

import re
from collections import defaultdict
from dataclasses import dataclass
from datetime import datetime, timedelta

from tools.perf.source import TICK_HZ, Rec, wall

LACKS = re.compile(r'a (?P<what>(?:compute )?state) the pack lacks (?P<how>made|refused|answered)\b(?P<rest>.*)')
IN_MS = re.compile(r' in (?P<ms>\d+) ms')
MADE = re.compile(
    r': (?P<what>a specialized function|a stitched library|a library from source) made by the shader '
    r'compiler in (?P<ms>\d+) ms'
)
COMPILER = re.compile(
    r'^SwitchShaderCompiler: made in (?P<ms>\d+) ms, queued (?P<q>\d+) ms .*?for pid (?P<pid>\d+): '
    r'(?P<kind>\w+)'
)
IMPORT = re.compile(r'^tegra-gm20b: slow import (?P<proc>\S+) (?P<kib>\d+) KiB surface \d+: (?P<us>\d+) us')
BEGIN = re.compile(r'begin_time#_##_#(?P<t>\d+)##')
BASE = re.compile(r'base_mct=(?P<t>\d+)')
PIPELINE = re.compile(r'string1#_##_#(?P<name>[^#]+)##')
COMMITS = {'Transaction', 'Transaction.Stalls'}
# Each is the span from the signpost's begin_time (base_mct for the render server's frame update) to the signpost.
STALLS = {
    ('com.apple.UIKit', 'UpdateCycle.Stalls'): 'UIKit update cycle over budget',
    ('com.apple.coreanimation', 'Transaction.Stalls'): 'Core Animation transaction, begin to commit',
    ('com.apple.coreanimation', 'IOMFBServer.Stalls'): 'render-server frame update, base time to update',
}


@dataclass(frozen=True)
class Cost:
    time: datetime  # when it ended: the line's time
    proc: str
    pid: int
    tid: int
    what: str
    ms: float

    @property
    def start(self) -> datetime:
        return self.time - timedelta(milliseconds=self.ms)


def compiles(recs: list[Rec]) -> list[Cost]:
    """Every creation, compile and slow import a line names, with what it cost the asking thread."""
    out = []
    for r in recs:
        if m := LACKS.search(r.msg):
            t = IN_MS.search(m['rest'])
            source = 'store' if "compiler's store" in m['rest'] else 'compiler'
            if t:
                out.append(Cost(r.time, r.proc, r.pid, r.tid, f'{m["what"]} {m["how"]} ({source})', float(t['ms'])))
        elif m := MADE.search(r.msg):
            out.append(Cost(r.time, r.proc, r.pid, r.tid, m['what'][2:], float(m['ms'])))
        elif m := COMPILER.match(r.msg):
            out.append(Cost(r.time, r.proc, int(m['pid']), r.tid, f'compiler side: {m["kind"]}', float(m['ms'])))
        elif m := IMPORT.match(r.msg):
            out.append(Cost(r.time, m['proc'], 0, 0, f'slow import ({m["kib"]} KiB)', int(m['us']) / 1000))
    return out


def ended(r: Rec, began: int, what: str) -> Cost:
    return Cost(r.time, r.proc, r.pid, r.tid, what, (r.mach - began) * 1000 / TICK_HZ)


def stalls(recs: list[Rec]) -> list[Cost]:
    """Apple's stall signposts and Core Animation's pipeline creations, each ending at its signpost."""
    out = []
    begun: dict[tuple[int, int, int], tuple[int, str]] = {}
    for r in recs:
        if not r.name:
            continue
        label = STALLS.get((r.subsystem, r.category))
        if label and (m := BEGIN.search(r.msg) or BASE.search(r.msg)):
            out.append(ended(r, int(m['t']), label))
        elif r.name == 'NewRenderPipeline' and r.kind == 'begin':
            name = PIPELINE.search(r.msg)
            begun[(r.pid, r.tid, r.ident)] = (r.mach, name['name'] if name else '?')
        elif r.name == 'NewRenderPipeline' and r.kind == 'end' and (b := begun.pop((r.pid, r.tid, r.ident), None)):
            out.append(ended(r, b[0], f'Core Animation pipeline {b[1]}'))
    return out


@dataclass(frozen=True)
class Row:
    proc: str
    what: str
    n: int
    total: float
    p50: float
    p90: float
    worst: float

    def text(self) -> str:
        return (
            f'{self.proc}: {self.n} x {self.what}, {self.total:.0f} ms in all (p50 {self.p50:.0f}, p90 {self.p90:.0f}, '
            f'max {self.worst:.0f})'
        )


def apple(c: Cost) -> bool:
    """Whether the cost is one of Apple's own marks rather than one of our lines."""
    return c.what in STALLS.values() or c.what.startswith('Core Animation pipeline')


def by_kind(costs: list[Cost]) -> list[Row]:
    """Return one row per process and kind of work, the costliest first."""
    groups: dict[tuple[str, str], list[float]] = defaultdict(list)
    for c in costs:
        what = 'Core Animation pipeline' if c.what.startswith('Core Animation pipeline') else c.what
        groups[(c.proc, what)].append(c.ms)
    rows = []
    for (proc, what), xs in groups.items():
        xs.sort()
        rows.append(Row(proc, what, len(xs), sum(xs), xs[len(xs) // 2], xs[len(xs) * 9 // 10], xs[-1]))
    rows.sort(key=lambda row: -row.total)
    return rows


def first_commit(recs: list[Rec], pid: int, start: datetime, end: datetime) -> tuple[datetime, datetime] | None:
    """Return when the process's first Core Animation commit in the span began its transaction and committed it.

    Commits are read from both of Core Animation's transaction categories: the plain one and the one for stalls.
    """
    for r in recs:
        mine = r.pid == pid and r.name == 'Commit' and r.category in COMMITS and start <= r.time <= end
        if mine and (m := BEGIN.search(r.msg)):
            return wall(r, int(m['t'])), r.time
    return None
