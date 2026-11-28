"""The scenarios of a session as windows: HW Test's tests and marks, launches, touches, hangs, Capture presses.

Every section of the report is read per window, so an open question about one moment (a launch, a snapshot, a hang)
gets one dossier of everything logged inside it.
"""

import re
from dataclasses import dataclass
from datetime import datetime, timedelta

from tools.perf.source import Rec, ms, wall

HWTEST = re.compile(r'^HWTEST (?P<id>\S+) (?P<verdict>PASS|FAIL|SKIP)\b.*?\bms=(?P<ms>[\d.]+)')
ASKED = re.compile(r'Asked to bootstrap a new process with identity: app<(?P<bundle>[^(>]+)')
BOOTSTRAPPED = re.compile(r'\[app<(?P<bundle>[^(>]+)>:(?P<pid>\d+)\] Bootstrap success')
READY = re.compile(r'sceneID:(?P<bundle>\S+?)-default\] scene content state changed: ready')
TOUCH = re.compile(r'SwitchFrames interaction: (?P<ms>\d+) ms \(touch (?P<touch>\d+) ms\)')
MARK = re.compile(r'SwitchMark: (?P<label>\S+)(?: (?P<ms>\d+) ms)?')
HANG_TYPE = re.compile(r'string2#_##_#(?P<type>[^#]+)##')
BEGIN = re.compile(r'begin_time#_##_#(?P<t>\d+)##')
END = re.compile(r'end_time#_##_#(?P<t>\d+)##')
HANG_LINE = re.compile(r'Hang detected: (?P<s>[\d.]+)s')
LAUNCH_LIMIT = timedelta(seconds=60)
PRESS = 'Snapshot button pressed down'
PRESS_BEFORE = timedelta(seconds=3)  # what the owner saw before pressing
PRESS_JOIN = timedelta(seconds=2)  # a recorder line this close to a press is about it
RECORDER_MARK = re.compile(r'SwitchRecorder: (?:capture (?P<seq>\d+) \(mark |refused mark .*?: (?P<why>\w+)$)')
SUITE_IDS = {'suite', 'suite.reds'}


@dataclass(frozen=True)
class Window:
    kind: str  # test, launch, touch, hang, mark
    name: str
    start: datetime
    end: datetime
    pid: int = 0
    proc: str = ''
    note: str = ''

    @property
    def ms(self) -> float:
        return (self.end - self.start).total_seconds() * 1000

    def holds(self, t: datetime) -> bool:
        return self.start <= t <= self.end

    def overlap(self, start: datetime, end: datetime) -> float:
        """Return the milliseconds of [start, end] inside the window."""
        lo, hi = max(start, self.start), min(end, self.end)
        return max(0.0, (hi - lo).total_seconds() * 1000)


def tests(recs: list[Rec]) -> list[Window]:
    """Each HW Test result line's test, from its start (the line's time less its ms=) to the line."""
    out = []
    for r in recs:
        m = HWTEST.match(r.msg) if r.msg.startswith('HWTEST ') else None
        if m and m['id'] not in SUITE_IDS:
            start = r.time - timedelta(milliseconds=float(m['ms']))
            out.append(Window('test', m['id'], start, r.time, r.pid, r.proc, m['verdict']))
    return out


def launches(recs: list[Rec]) -> list[Window]:
    """Each app FrontBoard was asked to start, from the ask to its scene's content turning ready."""
    asked: dict[str, datetime] = {}
    pids: dict[str, int] = {}
    out = []
    for r in recs:
        if r.subsystem != 'com.apple.FrontBoard':
            continue
        if m := ASKED.search(r.msg):
            asked.setdefault(m['bundle'], r.time)
        elif m := BOOTSTRAPPED.search(r.msg):
            pids[m['bundle']] = int(m['pid'])
        elif (m := READY.search(r.msg)) and m['bundle'] in asked:
            start = asked.pop(m['bundle'])
            if r.time - start <= LAUNCH_LIMIT:
                out.append(Window('launch', m['bundle'], start, r.time, pids.get(m['bundle'], 0), m['bundle']))
    out += [Window('launch', b, t, t, pids.get(b, 0), b, 'never ready in the log') for b, t in asked.items()]
    return out


def touches(recs: list[Rec]) -> list[Window]:
    """Each touch run the display logged, from its first contact to its close (a second after the last)."""
    found = [(r, m) for r in recs if r.proc == 'kernel' and (m := TOUCH.search(r.msg))]
    return [
        Window('touch', f'touch {m["touch"]} ms', r.time - timedelta(milliseconds=int(m['ms'])), r.time)
        for r, m in found
    ]


def marks(recs: list[Rec]) -> list[Window]:
    """Each scenario mark HW Test (or a kernel SwitchMark line) logged, with its span: not the owner's presses."""
    out = []
    for r in recs:
        if (r.proc == 'kernel' or r.subsystem == 'com.brunostjohn.switch.hwtest') and (m := MARK.match(r.msg)):
            span = timedelta(milliseconds=int(m['ms'] or 0))
            out.append(Window('mark', m['label'], r.time - span, r.time))
    return out


def presses(recs: list[Rec]) -> list[Window]:
    """Each Capture press of the owner's: SpringBoard's press line, with the recorder's capture or refusal of it.

    Without SpringBoard's line (a Bluetooth controller's Capture, or debug.switch_recorder_mark), the recorder's own
    mark line is the press. The window is the few seconds before it, what the owner saw.
    """
    kernel = [(r.time, m) for r in recs if r.proc == 'kernel' and (m := RECORDER_MARK.match(r.msg))]
    used: set[int] = set()
    out = []
    for r in recs:
        if r.proc != 'SpringBoard' or not r.msg.startswith(PRESS):
            continue
        note = 'no recorder line'
        for i, (t, m) in enumerate(kernel):
            if i not in used and abs(t - r.time) <= PRESS_JOIN:
                used.add(i)
                note = f'capture {m["seq"]}' if m['seq'] else f'refused ({m["why"]})'
                break
        out.append(Window('press', 'capture', r.time - PRESS_BEFORE, r.time, note=note))
    for i, (t, m) in enumerate(kernel):
        if i not in used:
            out.append(
                Window(
                    'press',
                    'mark',
                    t - PRESS_BEFORE,
                    t,
                    note=f'capture {m["seq"]}' if m['seq'] else f'refused ({m["why"]})',
                )
            )
    return sorted(out, key=lambda w: w.end)


def hangs(recs: list[Rec]) -> list[Window]:
    """Each main-thread hang HangTracer saw: its signpost's begin and end, or its log line when no signpost is kept."""
    out = []
    for r in recs:
        if r.name == 'HangInterval' and (b := BEGIN.search(r.msg)) and (e := END.search(r.msg)):
            kind = HANG_TYPE.search(r.msg)
            begin, end = int(b['t']), int(e['t'])
            out.append(
                Window(
                    'hang',
                    f'{ms(end - begin) / 1000:.2f} s',
                    wall(r, begin),
                    wall(r, end),
                    r.pid,
                    r.proc,
                    kind['type'] if kind else '',
                )
            )
    if out:
        return out
    for r in recs:
        if r.subsystem == 'com.apple.hangtracer' and (m := HANG_LINE.match(r.msg)):
            start = r.time - timedelta(seconds=float(m['s']))
            out.append(Window('hang', f'{m["s"]} s', start, r.time, r.pid, r.proc, 'ended at its log line'))
    return out


def inside(windows: list[Window], t: datetime) -> list[Window]:
    return [w for w in windows if w.holds(t)]
