"""Frame weights: where a captured frame's GPU time went, by compiled state, from the driver's "SwitchMetal: fw" lines.

Ask a debug build's process for a capture (backboardd draws the compositor's frames), while the screen is moving:

  idevicenotificationproxy -u <udid> post com.brunostjohn.switch.debug.frame-weights.backboardd

pull the log archive over USB (tools/devicerun/README.md), then on the Mac, with the pack of the build that ran:

  python3 -m tools.devicerun.frame_weights <absolute .logarchive> --pack <pack.bin> [--names <file>] [--top 20]

The capture ran each draw alone from an idle pipe (drivers/TegraGM20BMetal/draw_times.h): a state's
time is what its draws cost alone, which ranks states, and its command buffers took longer than the ones before it; the
report prints both. `--names` reads "<16-hex key> <label>" lines for states Core Animation did not name.
"""

import argparse
import re
import statistics
import sys
from collections.abc import Iterable
from dataclasses import dataclass, field
from pathlib import Path

from tools.devicerun import devicerun
from tools.gpu.maxwell import nakstat

PREDICATE = 'subsystem == "com.brunostjohn.switch.TegraGM20BMetal" AND eventMessage BEGINSWITH "SwitchMetal: fw c"'
LINE = re.compile(r'SwitchMetal: fw c(?P<serial>\d+)\.(?P<number>\d+) (?P<text>.*)$')
DRAW = re.compile(
    r'(?P<kind>[di])(?P<state>\d+|-):(?P<pass>\d+|-):(?P<topology>\d+|-):(?P<vertices>\d+)x(?P<instances>\d+):'
    r'(?P<box>\d+):(?P<invocations>\d+|-):(?P<ns>\d+)$'
)
OTHER = re.compile(r'(?P<kind>[sbpt]):(?P<pass>\d+|-):(?P<invocations>\d+|-):(?P<ns>\d+)$')
STATE = re.compile(
    r'state (?P<index>\d+) (?P<key>[0-9a-f]{16}) (?P<form>\S+) fs(?P<fs>\d+) vs(?P<vs>\d+) r(?P<gprs>\d+) '
    r'x(?P<passes>\d+) (?P<name>.+)$'
)
CB = re.compile(
    r'cb b(?P<buffer>\d+) s\d+ start (?P<start>\d+) gpu (?P<gpu>\d+) marks \d+ unwritten (?P<unwritten>\d+) '
    r'full (?P<full>\d+)'
)
DROPPED = re.compile(r'dropped (?P<buffer>b\d+ \S+)')
END = re.compile(r'end taken (?P<taken>\d+) reported (?P<reported>\d+) dropped (?P<dropped>\d+) lines (?P<lines>\d+)')
CAPTURE = re.compile(r'capture (?P<process>\S+) asked (?P<asked>\d+) stamp \d+ ref(?P<ref>(?: (?:\d+|-))*)')
# GM20B's issue rate: 2 SMs of 128 lanes at 768 MHz, one instruction a lane a clock (span_marks.h)
INSTRUCTIONS_PER_NS = 256 * 0.768
# A program's code words hold one scheduling word in every four (tools/gpu/maxwell/isa.py instructions)
INSTRUCTIONS_PER_WORD = 3 / 4
PTIMER_NS = 625 / 384  # a PTIMER unit (stamps.h)
HUGE_GPRS, HUGE_INSTRUCTIONS, OCCUPANCY_GPRS, NORMAL_GPRS = 64, 4000, 96, 32
KIND_NAMES = {'s': 'start', 'b': 'before passes', 'p': 'prologues', 't': 'tails'}


@dataclass
class Row:
    buffer: int
    kind: str
    state: int | None
    invocations: int | None
    box: int
    ns: int


@dataclass
class State:
    key: int
    form: str
    words: int
    gprs: int
    name: str


@dataclass
class Capture:
    serial: int
    process: str = '-'
    asked: int = 0
    reference: list[int | None] = field(default_factory=list)
    gpu: dict[int, int] = field(default_factory=dict)
    starts: dict[int, int] = field(default_factory=dict)
    unwritten: int = 0
    full: int = 0
    lost: list[str] = field(default_factory=list)
    states: dict[int, State] = field(default_factory=dict)
    rows: list[Row] = field(default_factory=list)
    numbers: set[int] = field(default_factory=set)
    lines: int | None = None
    dropped: int = 0

    def missing(self) -> list[int]:
        """Return the line numbers the archive lacks, up to the end line's count or the last line seen."""
        last = self.lines if self.lines is not None else max(self.numbers, default=-1) + 1
        return [n for n in range(last) if n not in self.numbers]


@dataclass
class Weight:
    label: str
    state: State
    draws: int = 0
    counted: int = 0  # draws whose invocations the counter gave
    invocations: int = 0
    box: int = 0
    ns: int = 0
    static: dict | None = None

    @property
    def boxed(self) -> bool:
        """Whether a draw of it had no counted invocations, so its pixels are the CPU's boxes instead."""
        return self.counted < self.draws

    @property
    def pixels(self) -> int:
        return self.box if self.boxed else self.invocations

    @property
    def instructions(self) -> float:
        if self.static and self.static['words'] == self.state.words:
            return float(self.static['instructions'])
        return self.state.words * INSTRUCTIONS_PER_WORD

    def ns_per_pixel(self) -> float:
        return self.ns / self.pixels if self.pixels else 0.0

    def predicted_ns_per_pixel(self) -> float:
        return self.instructions / INSTRUCTIONS_PER_NS

    def ratio(self) -> float:
        predicted = self.predicted_ns_per_pixel()
        return self.ns_per_pixel() / predicted if predicted else 0.0


def number(text: str) -> int | None:
    return None if text == '-' else int(text)


def entry(buffer: int, text: str) -> Row | None:
    if found := DRAW.match(text):
        return Row(
            buffer,
            found['kind'],
            number(found['state']),
            number(found['invocations']),
            int(found['box']),
            int(found['ns']),
        )
    if found := OTHER.match(text):
        return Row(buffer, found['kind'], None, number(found['invocations']), 0, int(found['ns']))
    return None


def add_line(c: Capture, text: str) -> None:
    head, _, rest = text.partition(' ')
    if head == 'd':
        at, _, entries = rest.partition(' ')
        c.rows.extend(r for r in (entry(int(at[1:]), e) for e in entries.split()) if r)
    elif found := STATE.match(text):
        c.states[int(found['index'])] = State(
            int(found['key'], 16), found['form'], int(found['fs']), int(found['gprs']), found['name']
        )
    elif found := CB.match(text):
        c.gpu[int(found['buffer'])] = int(found['gpu'])
        c.starts[int(found['buffer'])] = int(found['start'])
        c.unwritten += int(found['unwritten'])
        c.full += int(found['full'])
    elif found := DROPPED.match(text):
        c.lost.append(found['buffer'])
    elif found := CAPTURE.match(text):
        c.process, c.asked = found['process'], int(found['asked'])
        c.reference = [number(t) for t in found['ref'].split()]
    elif found := END.match(text):
        c.lines, c.dropped = int(found['lines']), int(found['dropped'])


def parse(records: Iterable[dict]) -> dict[tuple, Capture]:
    """Return each capture's lines put together, by boot, process ID and serial."""
    captures: dict[tuple, Capture] = {}
    for record in records:
        found = LINE.search(record.get('eventMessage') or '')
        if not found:
            continue
        serial = int(found['serial'])
        key = (record.get('bootUUID', ''), record.get('processID', 0), serial)
        c = captures.setdefault(key, Capture(serial))
        c.numbers.add(int(found['number']))
        add_line(c, found['text'])
    return captures


def weights(c: Capture, names: dict[int, str], statics: dict[tuple[int, str], dict]) -> list[Weight]:
    """Return the capture's drawn programs by GPU time, most first."""
    out: dict[int, Weight] = {}
    for r in c.rows:
        if r.state is None or r.state not in c.states:
            continue
        s = c.states[r.state]
        w = out.get(r.state)
        if w is None:
            label = s.name if s.name != '-' else names.get(s.key, f'k{s.key:016x}')
            w = out[r.state] = Weight(label, s, static=statics.get((s.key, s.form)))
        w.draws += 1
        w.counted += r.invocations is not None
        w.invocations += r.invocations or 0
        w.box += r.box
        w.ns += r.ns
    return sorted(out.values(), key=lambda w: w.ns, reverse=True)


def static_rows(pack_path: Path, keys: set[tuple[int, str]]) -> tuple[dict[tuple[int, str], dict], str]:
    """Return nakstat's row of the record each (key, form) names in the pack, and why there are none if so."""
    try:
        pack = nakstat.Pack(pack_path)
    except SystemExit as e:
        return {}, f'no static counts: {e}'
    out = {}
    for key, form in keys:
        found = (pack.free if form == 'fetchfree' else pack.pipelines).get(key)
        if not found or found[0] == nakstat.NO_PROGRAM:
            continue
        code = pack.code(found[0])
        row = nakstat.row('', code, pack.gprs(found[0]))
        row['words'] = len(code) // 8
        out[key, form] = row
    return out, ''


def ms(ns: float) -> str:
    return f'{ns / 1e6:.2f} ms'


def summary(c: Capture, ws: list[Weight]) -> list[str]:
    total = sum(r.ns for r in c.rows)
    known = [n for n in c.reference if n is not None]
    gaps = c.missing()
    lines = f'MISSING LINES {gaps[:8]}' if gaps else 'lines complete' if c.lines is not None else 'no end line yet'
    slower = f' (x{total / len(c.gpu) / (sum(known) / len(known)):.2f} each)' if known and c.gpu and sum(known) else ''
    invocations = sum(w.invocations for w in ws) / 1e6
    outside = ', '.join(f'{name} {ms(sum(r.ns for r in c.rows if r.kind == k))}' for k, name in KIND_NAMES.items())
    first = min(c.starts.values(), default=0)
    starts = ', '.join(f'{(v - first) * PTIMER_NS / 1e6:.2f}' for _, v in sorted(c.starts.items()))
    return [
        f'capture c{c.serial} {c.process}: {len(c.gpu)} of {c.asked} command buffers, {c.dropped} dropped, {lines}',
        (
            f'  isolated GPU time {ms(total)} over {len(c.gpu)} command buffers; '
            f'the {len(known)} before it took {ms(sum(known))}{slower}'
        ),
        f'  command buffers start at {starts} ms on the GPU' + (f'; dropped {", ".join(c.lost)}' if c.lost else ''),
        (
            f'  draws {sum(w.draws for w in ws)} of {len(ws)} programs, invocations {invocations:.2f} M, '
            f'unwritten marks {c.unwritten}'
            + (f', MARKS REFUSED {c.full} (their time is in the tails)' if c.full else '')
        ),
        f'  outside draws: {outside}',
    ]


def table(ws: list[Weight], total: int, top: int) -> list[str]:
    head = f'{"#":>3} {"state":42} {"form":9} {"r":>3} {"wrp":>3} {"draws":>5} {"kpx":>8} {"box kpx":>7} {"ms":>7} '
    out = [head + f'{"share":>6} {"ns/px":>7} {"instr":>6} {"pred":>6} {"x":>5}']
    for i, w in enumerate(ws[:top], 1):
        approx = '' if w.static and w.static['words'] == w.state.words else '~'
        counted = f'{w.invocations / 1000:7.0f}' + ('*' if w.boxed else ' ')
        out.append(
            f'{i:3} {w.label[:42]:42} {w.state.form:9} {w.state.gprs:3} {nakstat.warps_per_sm(w.state.gprs):3} '
            f'{w.draws:5} {counted} {w.box / 1000:7.0f} {w.ns / 1e6:7.3f} {100 * w.ns / total if total else 0:5.1f}% '
            f'{w.ns_per_pixel():7.3f} {approx}{w.instructions:5.0f} {w.predicted_ns_per_pixel():6.2f} {w.ratio():5.2f}'
        )
    out.append('kpx: the invocations counted; * where a draw had none, so ns/px divides by the CPU boxes')
    return out


def huge(ws: list[Weight], total: int) -> list[str]:
    """Return item 10's numbers: the share of programs whose registers cap occupancy, and their measured cost."""
    big = [w for w in ws if w.state.gprs >= HUGE_GPRS or w.instructions >= HUGE_INSTRUCTIONS]
    normal = [w.ratio() for w in ws if w.state.gprs <= NORMAL_GPRS and w.instructions < HUGE_INSTRUCTIONS and w.pixels]
    capped = sum(w.ns for w in ws if w.state.gprs >= OCCUPANCY_GPRS)
    median = statistics.median(normal) if normal else 0.0
    share = 100 * capped / total if total else 0.0
    out = [
        (
            f'item 10: programs of {OCCUPANCY_GPRS}+ registers took {share:.1f}% ({ms(capped)}); programs of '
            f'{NORMAL_GPRS} registers or fewer take x{median:.2f} their instruction-count time '
            f'(median of {len(normal)})'
        )
    ]
    for w in big:
        relative = w.ratio() / median if median else 0.0
        out.append(
            f'  {w.label[:42]:42} {w.state.form:9} r{w.state.gprs} warps {nakstat.warps_per_sm(w.state.gprs)} '
            f'draws {w.draws} kpx {w.pixels / 1000:.0f} ns/px {w.ns_per_pixel():.3f} instr {w.instructions:.0f} '
            f'x{w.ratio():.2f} ({relative:.2f} of the median)'
        )
    return out


def report(c: Capture, names: dict[int, str], statics: dict[tuple[int, str], dict], top: int) -> str:
    ws = weights(c, names, statics)
    total = sum(r.ns for r in c.rows)
    return '\n'.join([*summary(c, ws), '', *table(ws, total, top), '', *huge(ws, total)])


def read_names(path: Path | None) -> dict[int, str]:
    if not path:
        return {}
    out = {}
    for line in path.read_text().splitlines():
        key, _, label = line.strip().partition(' ')
        if re.fullmatch(r'[0-9a-f]{16}', key) and label:
            out[int(key, 16)] = label.strip()
    return out


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument(
        'archive', help='an absolute .logarchive path, or with --ndjson `log show --style ndjson` output'
    )
    parser.add_argument('--ndjson', action='store_true')
    parser.add_argument('--pack', type=Path, help="the build's pack.bin, for the static instruction counts")
    parser.add_argument('--names', type=Path)
    parser.add_argument('--capture', type=int, help='one capture serial (default: every capture)')
    parser.add_argument('--top', type=int, default=20)
    args = parser.parse_args(argv)
    records = (
        devicerun.read_ndjson(args.archive) if args.ndjson else devicerun.log_show(args.archive, PREDICATE, None, None)
    )
    captures = [c for c in parse(records).values() if args.capture in (None, c.serial)]
    if not captures:
        print('no "SwitchMetal: fw" lines: no capture was asked for, or a release build ran')
        return 1
    names = read_names(args.names)
    for c in captures:
        statics, why = {}, 'no static counts: no --pack'
        if args.pack:
            statics, why = static_rows(args.pack, {(s.key, s.form) for s in c.states.values()})
        print(report(c, names, statics, args.top))
        if why:
            print(f'  {why.upper()}: instruction columns are the device code words x 3/4 (~)')
        print()
    return 0


if __name__ == '__main__':
    sys.exit(main())
