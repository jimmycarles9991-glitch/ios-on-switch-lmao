"""The development DMA probe's lines (drivers/SwitchUSB/UsbDmaProbe*.cpp): its summaries, read as one record each.

A summary is a burst of lines with one occasion ("host session start", "device role", "controller off", "lines used
up"): the counts by path, then one line per descriptor class with the cache modes it arrived in, then the live
instances of the USB host classes. Held mappings that must be completed per transfer are the stale-data hazard: with
per-transfer mapping they must retire at teardown. The controller's own buffers stay mapped while it is
powered off, so their count at each power-off is level when nothing leaks.
"""

import re
from datetime import timedelta

from tools.devicerun.rules import FAIL, INFO, NO_DATA, PASS, REPORT, Check, Result, clock

OWNER = 'usb-host'
PROBE = 'drivers/SwitchUSB/UsbDmaProbe.cpp, drivers/SwitchUSB/UsbDmaProbeHeld.cpp'
WRITE_COMBINED = 0x10
CONTROLLER_BUFFERS = 'AppleUSBHostDMABufferMemoryDescriptor'
GIB_4 = 1 << 32
PREFIX = r'^SwitchUSB: DMA probe summary \((?P<when>[^)]*)\): '
HEADER = re.compile(
    PREFIX + r'(?P<wrong>\d+) held mappings that must be completed per transfer; maps/unmaps/held: own address '
    r'(?P<own>\d+/\d+/\d+), window (?P<window>\d+/\d+/\d+), bounced (?P<bounced>\d+/\d+/\d+); (?P<untracked>\d+) maps '
    r'untracked \(table full\), (?P<unknown>\d+) unmaps of no tracked mapping'
)
CLASS = re.compile(
    PREFIX + r'(?P<name>\w+): (?P<maps>\d+) maps, (?P<unmaps>\d+) unmaps, (?P<held>-?\d+) held; cache modes '
    r'(?P<modes>0x[0-9a-f]+|0)'
)
LIVE = re.compile(PREFIX + r'live:(?P<live>.*)$')
BUFFER = re.compile(
    r'^SwitchUSB: DMA probe #\d+ (?P<kind>prepare|complete) (?P<path>.+?) on CPU \d+: .*? pa (?P<pa>0x[0-9a-f]+)'
)
BURST = timedelta(seconds=1)


def summaries(lines: list[dict]) -> list[dict]:
    """Return the probe's summaries in time order, each its header's counts with its class and live lines."""
    found: list[dict] = []
    for m in sorted(lines, key=lambda m: m['time']):
        text = m['message']
        header, cls, live = HEADER.search(text), CLASS.search(text), LIVE.search(text)
        if header:
            paths = {p: tuple(int(n) for n in header[p].split('/')) for p in ('own', 'window', 'bounced')}
            found.append(
                {
                    'time': m['time'],
                    'when': header['when'],
                    'wrong': int(header['wrong']),
                    'paths': paths,
                    'untracked': int(header['untracked']),
                    'unknown': int(header['unknown']),
                }
                | {'classes': {}, 'live': {}, 'line': m}
            )
            continue
        current = found[-1] if found and m['time'] - found[-1]['time'] <= BURST else None
        if current is None or not (cls or live) or (cls or live)['when'] != current['when']:
            continue
        if cls:
            current['classes'][cls['name']] = {k: int(cls[k], 0) for k in ('maps', 'unmaps', 'held', 'modes')}
        else:
            pairs = live['live'].split()
            current['live'] = {name: int(n) for name, n in zip(pairs[::2], pairs[1::2], strict=False)}
    return found


def family_buffer(name: str) -> bool:
    return name.startswith('AppleUSBHost') and name.endswith('BufferMemoryDescriptor')


def family_modes(found: list[dict]) -> dict[str, int]:
    """Return each family buffer class's cache modes over every summary (bit n: mode n)."""
    modes: dict[str, int] = {}
    for summary in found:
        for name, counts in summary['classes'].items():
            if family_buffer(name):
                modes[name] = modes.get(name, 0) | counts['modes']
    return modes


def controller_held(found: list[dict]) -> list[int]:
    """Return how many of the controller's own buffers were held at each power-off."""
    offs = [s for s in found if s['when'] == 'controller off' and CONTROLLER_BUFFERS in s['classes']]
    return [s['classes'][CONTROLLER_BUFFERS]['held'] for s in offs]


def problems(found: list[dict]) -> list[str]:
    """Report outstanding transfers, cached controller buffers, and controller buffer leaks."""
    out = [
        f'{s["wrong"]} held mappings to complete per transfer at {s["when"]} {clock(s["time"])}'
        for s in found
        if s['wrong'] and s['when'] in ('device role', 'controller off', 'host session start')
    ]
    controller_modes = family_modes(found).get(CONTROLLER_BUFFERS)
    if controller_modes is not None and controller_modes != WRITE_COMBINED:
        out.append(f'{CONTROLLER_BUFFERS} in cache modes {controller_modes:#x}, expected mode 4')
    held = controller_held(found)
    if len(set(held)) > 1:
        out.append(f'{CONTROLLER_BUFFERS} held at each controller off: {" > ".join(map(str, held))}')
    return out


def probe_lines(by_check: dict[str, Result]) -> list[dict]:
    return by_check['usb.dma.probe'].matches if 'usb.dma.probe' in by_check else []


def probe_row(by_check: dict[str, Result]) -> Result:
    """Judge completed sessions against payload ownership and controller buffer lifetimes."""
    rule = '0 held mappings to complete per transfer; controller DMA buffers in mode 4 only; controller buffers level'
    check = Check('usb.dma.probe-summary', OWNER, 'SwitchUSB: DMA probe summary', rule, source=PROBE)
    lines = probe_lines(by_check)
    found = summaries(lines)
    if not found:
        last = max((m for m in lines if 'kind' not in m), key=lambda m: m['time'], default=None)
        if last is None:
            return Result(check, NO_DATA, 'no DMA probe summary (a development build with the probe)')
        return Result(check, INFO, f'summary format not recognised; the last as printed: {last["message"]}')
    wrong = problems(found)
    if wrong:
        return Result(check, FAIL, '; '.join(wrong), [s['line'] for s in found])
    gaps = [s for s in found if s['untracked'] or s['unknown']]
    if gaps:
        last = gaps[-1]
        return Result(
            check,
            NO_DATA,
            f'incomplete DMA trace: {last["untracked"]} untracked maps, {last["unknown"]} unaccounted unmaps',
            [last['line']],
        )
    if not any(s['when'] in ('controller off', 'device role') for s in found):
        return Result(check, NO_DATA, 'waiting for a completed host session')
    families = family_modes(found)
    held = controller_held(found)
    observed = f'{len(found)} summaries: 0 held mappings to complete per transfer; '
    observed += (
        ', '.join(f'{name} cache modes {bits:#x}' for name, bits in sorted(families.items())) or 'no family buffer'
    )
    observed += f'; controller buffers {held[0]} held at each of {len(held)} controller offs' if held else ''
    return Result(check, PASS, observed, [s['line'] for s in found])


def window_row(by_check: dict[str, Result]) -> Result:
    """Return the host window's use, and the probe's prepare and complete lines with a buffer above 4 GiB by path."""
    check = Check('usb.dma.window', OWNER, 'SwitchUSB: DMA probe', REPORT, source=PROBE)
    found = summaries(probe_lines(by_check))
    buffers = [b for b in (BUFFER.search(m['message']) for m in probe_lines(by_check)) if b]
    sizes = by_check['usb.dma.host-window'].matches if 'usb.dma.host-window' in by_check else []
    if not found and not buffers:
        return Result(check, NO_DATA, 'no DMA probe line')
    parts = [f'a {sizes[-1]["pages"]}-page window'] if sizes else []
    if found:
        maps, unmaps, held = found[-1]['paths']['window']
        most = max(found, key=lambda s: s['paths']['window'][2])
        parts.append(
            f'window maps/unmaps/held {maps}/{unmaps}/{held} at the last summary, at most {most["paths"]["window"][2]} '
            f'held ({most["when"]} {clock(most["time"])})'
        )
    for kind in ('prepare', 'complete'):
        mine = [b for b in buffers if b['kind'] == kind]
        paths = sorted({b['path'] for b in mine})
        high = [(path, sum(int(b['pa'], 16) >= GIB_4 for b in mine if b['path'] == path)) for path in paths]
        total = {path: sum(b['path'] == path for b in mine) for path in paths}
        listed = ', '.join(f'{path} {n} of {total[path]}' for path, n in high)
        parts.append(f'{kind} lines with the buffer above 4 GiB: {listed or "none"}')
    return Result(check, INFO, '; '.join(parts))
