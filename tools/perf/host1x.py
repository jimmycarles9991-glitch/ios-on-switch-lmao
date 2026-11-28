"""host1x's command channels: how much of each channel's clocked time went on waiting for a syncpoint.

A channel runs its jobs in order, so a wait on one job's producer also sits inside every later queued job's submit to
done time, while the stall counter counts it once. The stall is therefore a share of clocked time, never subtracted
from an engine's job time; the rest is the channel idle, queued or running.
"""

import re
from dataclasses import dataclass
from datetime import datetime

from tools.perf.source import Rec

CHANNELS = ('NVDEC', 'VIC', 'NVENC', 'NVJPG', 'DC')
LINE = re.compile(
    r'^TegraHost1x: (?P<s>\d+)s NVDEC/VIC/NVENC/NVJPG/DC gathered=(?P<gathered>[\d/]+) copied=(?P<copied>[\d/]+) '
    r'.*?(?: clocked=(?P<clocked>[\d/]+)us stalled=(?P<stalled>[\d/]+)us| (?P<off>counters=off))?$'
)


@dataclass
class Channel:
    windows: int = 0
    seconds: int = 0
    jobs: int = 0
    counted: int = 0  # windows whose counters were read
    clocked_us: int = 0
    stalled_us: int = 0
    worst_share: float = 0.0
    worst_at: datetime | None = None


def _split(field: str) -> list[int]:
    return [int(v) for v in field.split('/')]


def read(recs: list[Rec]) -> dict[str, Channel]:
    out = {name: Channel() for name in CHANNELS}
    for r in recs:
        if r.proc != 'kernel' or not (m := LINE.match(r.msg)):
            continue
        jobs = [g + c for g, c in zip(_split(m['gathered']), _split(m['copied']), strict=True)]
        clocked = _split(m['clocked']) if m['clocked'] else None
        stalled = _split(m['stalled']) if m['stalled'] else None
        for i, name in enumerate(CHANNELS):
            ch = out[name]
            ch.windows += 1
            ch.seconds += int(m['s'])
            ch.jobs += jobs[i]
            if clocked is None or stalled is None:
                continue
            ch.counted += 1
            ch.clocked_us += clocked[i]
            ch.stalled_us += stalled[i]
            share = stalled[i] / clocked[i] if clocked[i] else 0.0
            if share > ch.worst_share:
                ch.worst_share, ch.worst_at = share, r.time
    return out


def lines(chs: dict[str, Channel]) -> list[str]:
    if not any(ch.windows for ch in chs.values()):
        return ['host1x channels: no TegraHost1x stats lines']
    if not any(ch.counted for ch in chs.values()):
        return ['host1x channels: no tick or stall counters in this kernel (counters=off or an older kernel)']
    out = []
    for name, ch in chs.items():
        if not ch.jobs and not ch.stalled_us:
            continue
        head = f'{name}: {ch.jobs} jobs over {ch.windows} windows ({ch.seconds} s)'
        if not ch.counted:
            out.append(f'{head}; counters off in every window')
        elif not ch.clocked_us:
            out.append(f'{head}; counters not running (0 clocked over {ch.counted} windows)')
        else:
            per_job = f', {ch.stalled_us / ch.jobs / 1000:.2f} ms a job' if ch.jobs else ''
            worst = f', worst window {ch.worst_share:.0%} at {ch.worst_at:%H:%M:%S}' if ch.worst_at else ''
            out.append(
                f'{head}; host1x clocked {ch.clocked_us / 1e6:.1f} s, waited on a syncpoint '
                f'{ch.stalled_us / 1e6:.2f} s ({ch.stalled_us / ch.clocked_us:.1%} of clocked{per_job}){worst}; '
                'the rest is idle, queued or running'
            )
    return out or ['host1x channels: no jobs and no waits']
