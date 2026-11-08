"""What a long step of //:image, //:image_to_card or //deploy:sd is doing, while it does it.

On a terminal the innermost running step owns one line, redrawn in place: its name, how much of how much is done, a
bar, the rate and the time left. Anywhere else (CI, a pipe) a plain line every PLAIN_EVERY seconds stands in for it.
Neither reaches the run's log file: host.Log prints through println() and writes only the lines a step logs.

Nothing here runs on a thread of its own. The code that waits on a copy or a tool calls advance(), apply() or tick()
from the main thread; with no step running they do nothing.
"""

import contextlib
import os
import re
import shutil
import sys
import time
from collections import deque
from collections.abc import Callable, Iterator
from dataclasses import dataclass, field
from typing import TextIO

BYTES = 'bytes'
PERCENT = '%'
WAIT = 0.25
DRAW_EVERY = 0.1
PLAIN_EVERY = 5.0
RATE_WINDOW = 10.0
RATE_SPAN = 1.0
BAR_WIDTH = 24
SIZE_UNITS = ('B', 'KiB', 'MiB', 'GiB', 'TiB')


@dataclass(frozen=True)
class Update:
    """What one line of a tool's output says of its progress.

    A note of '' clears the step's note. `keep`: the line is also the tool's own output (a phase it reports), kept in
    what the call returns.
    """

    done: float | None = None
    total: float | None = None
    unit: str | None = None
    note: str | None = None
    keep: bool = False


Parser = Callable[[str], Update | None]


class Rate:
    """Units per second over the last RATE_WINDOW seconds, once RATE_SPAN seconds of samples are in."""

    def __init__(self) -> None:
        self.samples: deque[tuple[float, float]] = deque()

    def add(self, now: float, done: float) -> None:
        self.samples.append((now, done))
        while len(self.samples) > 2 and now - self.samples[1][0] >= RATE_WINDOW:
            self.samples.popleft()

    def per_second(self) -> float | None:
        if len(self.samples) < 2:
            return None
        (start, first), (end, last) = self.samples[0], self.samples[-1]
        return (last - first) / (end - start) if end - start >= RATE_SPAN else None


def _scaled(n: float) -> tuple[float, int]:
    scale = 0
    while abs(n) >= 1024 and scale < len(SIZE_UNITS) - 1:
        n /= 1024
        scale += 1
    return n, scale


def _number(n: float, scale: int) -> str:
    return f'{n:.0f}' if scale == 0 or abs(n) >= 10 else f'{n:.1f}'


def size(n: float) -> str:
    """Return `n` bytes in binary units: 512 B, 1.5 MiB, 12 GiB."""
    value, scale = _scaled(n)
    return f'{_number(value, scale)} {SIZE_UNITS[scale]}'


def amount(done: float, total: float | None, unit: str) -> str:
    """Return how much is done, of the total when known: 1.2/4.5 GiB, 120/3413 files."""
    if unit != BYTES:
        return f'{done:.0f}/{total:.0f} {unit}' if total else f'{done:.0f} {unit}'
    if not total:
        return size(done)
    value, scale = _scaled(total)
    return f'{_number(done / 1024**scale, scale)}/{_number(value, scale)} {SIZE_UNITS[scale]}'


def per_second(rate: float, unit: str) -> str:
    if unit == BYTES:
        return f'{size(rate)}/s'
    return f'{rate:.1f} {unit}/s' if rate < 10 else f'{rate:.0f} {unit}/s'


def clock(seconds: float) -> str:
    """Return a duration as m:ss, or h:mm:ss from an hour."""
    hours, rest = divmod(round(seconds), 3600)
    minutes, secs = divmod(rest, 60)
    return f'{hours}:{minutes:02}:{secs:02}' if hours else f'{minutes}:{secs:02}'


@dataclass
class Bar:
    """One running step: what it is, how far it has got, and since when. A unit of None counts nothing."""

    name: str
    total: float | None
    unit: str | None
    started: float
    probe: Callable[[], float] | None = None
    done: float = 0
    note: str | None = None
    rate: Rate = field(default_factory=Rate)

    def add(self, now: float, n: float) -> None:
        self.done += n
        self.rate.add(now, self.done)

    def set(self, now: float, update: Update) -> None:
        if update.unit is not None and update.unit != self.unit:
            self.unit, self.rate = update.unit, Rate()
        if update.total is not None:
            self.total = update.total
        if update.done is not None:
            self.done = update.done
            self.rate.add(now, self.done)
        if update.note is not None:
            self.note = update.note or None

    def fraction(self) -> float | None:
        return min(max(self.done / self.total, 0.0), 1.0) if self.total and self.unit else None

    def eta(self) -> float | None:
        rate = self.rate.per_second()
        if self.fraction() is None or not rate or rate <= 0:
            return None
        return max(self.total - self.done, 0) / rate

    def facts(self, now: float) -> list[str]:
        """Return the step's amount, rate and time (left when known, else so far)."""
        facts = []
        counted = self.unit not in (None, PERCENT)
        if counted and (self.total or self.done):
            facts.append(amount(self.done, self.total, self.unit))
        rate = self.rate.per_second()
        if counted and rate and rate > 0:
            facts.append(per_second(rate, self.unit))
        eta = self.eta()
        facts.append(f'{clock(eta)} left' if eta is not None else clock(now - self.started))
        return facts

    def line(self, now: float, width: int) -> str:
        """Return the step as one terminal line of at most `width` - 1 columns."""
        parts = [self.name]
        fraction = self.fraction()
        if fraction is not None:
            filled = round(fraction * BAR_WIDTH)
            parts.append('[{}{}] {:3.0f}%'.format('#' * filled, '-' * (BAR_WIDTH - filled), 100 * fraction))
        parts += self.facts(now)
        if self.note:
            parts.append(self.note)
        return '  '.join(parts)[: max(width - 1, 0)]

    def plain(self, now: float) -> str:
        """Return the step as a plain line, for output that is not a terminal."""
        fraction = self.fraction()
        facts = [f'{100 * fraction:.0f}%'] if fraction is not None else []
        facts += self.facts(now)
        if self.note:
            facts.append(self.note)
        return '  {}: {}'.format(self.name, ', '.join(facts))


class Screen:
    """Where the innermost running step is shown: one terminal line drawn over, or a plain line now and then."""

    def __init__(self, out: TextIO | None = None, tty: bool | None = None, now: Callable[[], float] = time.monotonic):
        self._out, self._tty, self.now = out, tty, now
        self.steps: list[Bar] = []
        self.drawn = False
        self.last_draw = self.last_plain = self.last_probe = 0.0

    @property
    def out(self) -> TextIO:
        return self._out or sys.stdout

    @property
    def tty(self) -> bool:
        if self._tty is not None:
            return self._tty
        return self.out.isatty() and os.environ.get('TERM') != 'dumb'

    def push(self, bar: Bar) -> None:
        self.steps.append(bar)
        self.last_plain = bar.started
        self.draw()

    def pop(self, bar: Bar) -> None:
        self.erase()
        self.steps.remove(bar)
        self.draw()

    def println(self, text: str) -> None:
        self.erase()
        self.out.write(text + '\n')
        self.draw()

    def tick(self) -> None:
        if not self.steps:
            return
        now, bar = self.now(), self.steps[-1]
        if bar.probe and now - self.last_probe >= WAIT:
            self.last_probe = now
            with contextlib.suppress(OSError):
                bar.set(now, Update(done=bar.probe()))
        if self.tty:
            if now - self.last_draw >= DRAW_EVERY:
                self.draw(now)
        elif now - self.last_plain >= PLAIN_EVERY:
            self.last_plain = now
            self.out.write(bar.plain(now) + '\n')
            self.out.flush()

    def draw(self, now: float | None = None) -> None:
        if self.steps and self.tty:
            now = self.now() if now is None else now
            line = self.steps[-1].line(now, shutil.get_terminal_size().columns)
            self.out.write('\r' + line + '\x1b[K')
            self.drawn, self.last_draw = True, now
        self.out.flush()

    def erase(self) -> None:
        if self.drawn:
            self.out.write('\r\x1b[K')
            self.drawn = False


SCREEN = Screen()


@contextlib.contextmanager
def step(
    name: str, total: float | None = None, unit: str | None = BYTES, probe: Callable[[], float] | None = None
) -> Iterator[Bar]:
    """Show `name` while the block runs; `probe`, if given, is polled for how much is done."""
    bar = Bar(name, total, unit, SCREEN.now(), probe)
    SCREEN.push(bar)
    try:
        yield bar
    finally:
        SCREEN.pop(bar)


def advance(n: float) -> None:
    """Count `n` more units done toward the running step."""
    if SCREEN.steps:
        SCREEN.steps[-1].add(SCREEN.now(), n)
        SCREEN.tick()


def apply(update: Update) -> None:
    if SCREEN.steps:
        SCREEN.steps[-1].set(SCREEN.now(), update)
        SCREEN.tick()


def note(text: str) -> None:
    """Say what the running step is doing right now."""
    apply(Update(note=text))


def running(command: str) -> None:
    """Say which command a step that counts nothing of its own is waiting on."""
    if SCREEN.steps and SCREEN.steps[-1].fraction() is None:
        note(command)


def tick() -> None:
    """Redraw the running step if it is due; for code that waits."""
    SCREEN.tick()


def println(line: str) -> None:
    """Print a line of the run's own output above the running step."""
    SCREEN.println(line)


SEALED = re.compile(r'done (\d+)% of the fs blocks')
PUPPET_PERCENT = re.compile(r'PERCENT:(-?\d+(?:\.\d+)?)')
OURS = re.compile(r'progress (\d+)/(\d+) (\S.*)')
OURS_NOTE = 'progress '


def sealer(line: str) -> Update | None:
    """Read apfs_prepare_cryptex -p: '** Sealing volume disk5s1: 1435 hash records added, done 68% of the fs blocks'."""
    found = SEALED.search(line)
    return Update(done=int(found[1]), total=100, unit=PERCENT) if found else None


def puppetstrings(line: str) -> Update | None:
    """Read hdiutil -puppetstrings: 'PERCENT:63.529705' (-1 when it has no estimate) and 'MESSAGE:...'."""
    found = PUPPET_PERCENT.fullmatch(line.strip())
    if found:
        value = float(found[1])
        return Update(done=value, total=100, unit=PERCENT) if value >= 0 else Update()
    if line.startswith('MESSAGE:'):
        return Update(note=line[len('MESSAGE:') :].strip())
    return None


def ours(line: str) -> Update | None:
    """Read what our own tools print under --progress: 'progress DONE/TOTAL UNIT' or 'progress WHAT IT IS DOING'."""
    found = OURS.fullmatch(line.strip())
    if found:
        unit = found[3]
        return Update(done=int(found[1]), total=int(found[2]), unit=unit, note='')
    if line.startswith(OURS_NOTE):
        return Update(note=line[len(OURS_NOTE) :].strip())
    return None


def phases(line: str) -> Update | None:
    """Read a tool that only says which phase it is in (fsck_apfs, diskutil): each line is the note, and output."""
    text = line.strip().lstrip('*').strip()
    return Update(note=text, keep=True) if text else None
