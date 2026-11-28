"""A flight-recorder dump file, Switch-recorder-<slot>.rec (kernel/switch/recorder/store.c).

A 1024-byte text header, "SwitchRecorder 2 key=value ...", written last, then the capture's stackshot (kcdata) and its
ring (RAW_VERSION1 kdebug) as one raw deflate stream. A file pulled while the kernel wrote it has no header yet.
"""

from __future__ import annotations

import re
import zlib
from collections import Counter
from dataclasses import dataclass
from datetime import datetime, timedelta

from tools.perf.source import TICK_HZ, Rec

HEADER = 1024
MAGIC = 'SwitchRecorder 2 '
RING_LOST = {'1': 'kdebug was off or another process owned it', '2': 'no memory for it'}


@dataclass
class Dump:
    path: str
    head: dict[str, str]
    shot: bytes = b''
    ring: bytes = b''
    problem: str = ''

    def number(self, key: str) -> int:
        try:
            return int(self.head.get(key, '0'))
        except ValueError:
            return 0

    def after_trigger_ms(self, key: str) -> float | None:
        """Return how long after the trigger a step ended (shot_at, frozen_at, done_at), None when it did not."""
        at, then = self.number('at'), self.number(key)
        return (then - at) * 1000 / TICK_HZ if at and then >= at else None

    def describe(self) -> str:
        h = self.head
        steps = []
        for name, key in (('stackshot', 'shot_at'), ('ring frozen', 'frozen_at'), ('both in memory', 'done_at')):
            if (ms := self.after_trigger_ms(key)) is not None:
                steps.append(f'{name} +{ms:.1f} ms')
        lost = RING_LOST.get(h.get('ring_lost', '0'))
        split = (
            f'; its stretch: on a core {self.number("on_ms")} ms, blocked {self.number("blocked_ms")} ms, waiting for '
            f'a core {self.number("runnable_ms")} ms'
            if h.get('reason') == 'hang'
            else ''
        )
        return (
            f'capture {h.get("seq", "?")} of boot {h.get("boot", "?")[:8]} (ask {h.get("ask", "?")}): '
            + ', '.join(steps)
            + f'; stackshot {self.number("shot_us") / 1000:.1f} ms, ring read {self.number("ring_us") / 1000:.1f} ms'
            + (f'; no ring: {lost}' if lost else '')
            + split
        )


def fields(text: str) -> dict[str, str]:
    out = {}
    for token in text.split():
        key, eq, value = token.partition('=')
        if eq:
            out[key] = value
    return out


def read_head(path: str) -> dict[str, str]:
    """Return a dump file's header fields alone, {} when it has none yet."""
    try:
        with open(path, 'rb') as f:
            head = f.read(HEADER).decode('ascii', 'replace')
    except OSError:
        return {}
    return fields(head) if head.startswith(MAGIC) else {}


def read(path: str) -> Dump:
    with open(path, 'rb') as f:
        data = f.read()
    head = data[:HEADER].decode('ascii', 'replace')
    if not head.startswith(MAGIC):
        return Dump(path, {}, problem='no header: written while it was pulled, or not a recorder dump')
    h = fields(head)
    d = Dump(path, h)
    packed = d.number('deflate')
    if len(data) < HEADER + packed:
        d.problem = f'the body is {len(data) - HEADER} bytes of {packed}'
        return d
    inflater = zlib.decompressobj(-zlib.MAX_WBITS)
    try:
        body = inflater.decompress(data[HEADER : HEADER + packed]) + inflater.flush()
    except zlib.error as exc:
        d.problem = f'the body does not inflate ({exc})'
        return d
    shot, ring = d.number('shot'), d.number('ring')
    if not inflater.eof or len(body) != shot + ring:
        d.problem = f'the body inflates to {len(body)} bytes, not shot {shot} + ring {ring}'
        return d
    d.shot, d.ring = body[:shot], body[shot:]
    return d


# The recorder's lines (kernel/switch/recorder/capture.c, store.c).
CAPTURE = re.compile(
    r'SwitchRecorder: capture (?P<seq>\d+) \((?P<reason>\w+) ask (?P<ask>\d+), thread (?P<tid>\d+), value '
    r'(?P<value>\d+)\) at (?P<at>\d+): stackshot (?P<shot_kib>\d+) KiB in (?P<shot_us>\d+) us \(\+(?P<shot_after>\d+) '
    r'us\), ring (?P<ring_kib>\d+) KiB in (?P<ring_us>\d+) us \(\+(?P<frozen_after>\d+) us\), in memory '
    r'\+(?P<done_after>\d+) us(?:; no ring: (?P<lost>[^;]+))?(?:; its stretch: on a core (?P<on_ms>\d+) ms, blocked '
    r'(?P<blocked_ms>\d+) ms, waiting for a core (?P<runnable_ms>\d+) ms)?'
)
REFUSED = re.compile(
    r'SwitchRecorder: refused (?P<reason>\w+) ask (?P<ask>\d+) \(thread (?P<tid>\d+), value (?P<value>\d+)\) at '
    r'(?P<at>\d+): (?P<why>\w+)'
)
WRITTEN = re.compile(
    r'SwitchRecorder: dump (?P<seq>\d+) \((?P<reason>\w+) ask (?P<ask>\d+), thread (?P<tid>\d+), value '
    r'(?P<value>\d+)\) in slot (?P<slot>\d+): .*? as (?P<kib>\d+) KiB, deflate (?P<deflate_us>\d+) us, write '
    r'(?P<write_us>\d+) us, (?P<age_ms>\d+) ms after its trigger'
)


def quantile(values: list[float], q: float) -> float:
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(q * len(ordered)))] if ordered else 0.0


def recorder_lines(recs: list[Rec]) -> list[str]:
    """Return what the recorder's own lines say: captures by reason and how soon, refusals by cause, writes."""
    caps = [m for r in recs if (m := CAPTURE.search(r.msg))]
    refused = [m for r in recs if (m := REFUSED.search(r.msg))]
    written = [m for r in recs if (m := WRITTEN.search(r.msg))]
    if not caps and not refused and not written:
        return []
    by_reason = Counter(m['reason'] for m in caps)
    done = [int(m['done_after']) / 1000 for m in caps]
    out = [
        f'captures: {len(caps)} ('
        + ', '.join(f'{k} {v}' for k, v in by_reason.most_common())
        + ')'
        + (f'; in memory p50 +{quantile(done, 0.5):.1f} ms, max +{max(done):.1f} ms after the trigger' if done else ''),
    ]
    if lost := Counter(m['lost'] for m in caps if m['lost']):
        out.append('  captures without a ring: ' + ', '.join(f'{k} {v}' for k, v in lost.most_common()))
    if refused:
        why = Counter((m['reason'], m['why']) for m in refused)
        out.append(
            'refused (one line a second per reason and thread): '
            + ', '.join(f'{reason} {cause} {n}' for (reason, cause), n in why.most_common())
        )
    if written:
        ages = [int(m['age_ms']) / 1000 for m in written]
        out.append(
            f'written: {len(written)}, {quantile(ages, 0.5):.1f} s after their trigger at the median, '
            f'{max(ages):.1f} s at most; {sum(int(m["kib"]) for m in written) // 1024} MiB on the card'
        )
    return out


def busy(recs: list[Rec]) -> list[tuple[datetime, datetime, str]]:
    """Return when the recorder itself worked: each capture (trigger to in memory) and each write, as wall times.

    A capture line is logged once the capture is in memory, a dump line once its file is written.
    """
    out = []
    for r in recs:
        if m := CAPTURE.search(r.msg):
            out.append((r.time - timedelta(microseconds=int(m['done_after'])), r.time, f'capture {m["seq"]}'))
        elif m := WRITTEN.search(r.msg):
            took = int(m['deflate_us']) + int(m['write_us'])
            out.append((r.time - timedelta(microseconds=took), r.time, f'the write of capture {m["seq"]}'))
    return out
