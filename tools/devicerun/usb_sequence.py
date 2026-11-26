"""Rows that judge the order of the USB lines rather than their count.

They cover the detach, the controller's power after the host transport, the SuperSpeed reset, the descriptor reads of
each enumeration attempt, each configured device's driver (usb_driver.py), and the controller's recoveries around a
wake.
"""

import re
from collections import Counter
from datetime import datetime, timedelta

from tools.devicerun.rules import FAIL, INFO, NO_DATA, PASS, REPORT, Check, Result
from tools.devicerun.usb_controller import WAKE_S
from tools.devicerun.usb_driver import driver_row
from tools.devicerun.usb_port import REMOVED_IN_USE, ROOT_PORT

OWNER = 'usb-host'
ON = 3
CONTROLLER_S = 10.0
SS_ANSWER_S = 5.0
PP = 1 << 9


def row(name: str, verdict: str, observed: str, rule: str = REPORT) -> Result:
    return Result(Check(name, OWNER, '', rule), verdict, observed)


def matches(by_check: dict[str, Result], name: str) -> list[dict]:
    return sorted(by_check[name].matches if name in by_check else [], key=lambda m: m['time'])


def device_events(by_check: dict[str, Result]) -> list[tuple]:
    """Return the root-port devices' arrivals and departures as (time, kind, port)."""
    root = re.compile(ROOT_PORT)
    events = [(m['time'], 'add', m['port']) for m in matches(by_check, 'usb.enum.root')]
    events += [
        (m['time'], 'gone', m['port']) for m in matches(by_check, 'usb.enum.destroyed') if root.fullmatch(m['port'])
    ]
    return events


def detach_row(by_check: dict[str, Result]) -> Result:
    """Return whether a host transport left while a root-port device was still there and nothing kept it."""
    rule = 'no host transport removed under a device'
    ours = matches(by_check, 'usb.port.transports')
    if ours:
        return detach_by_our_lines(device_events(by_check), ours, rule)
    removed = matches(by_check, 'usb.port.removed')
    if not removed:
        return row('usb.port.detach-order', NO_DATA, 'no transport lines', rule)
    present, early = set(), []
    for time, what, port in sorted(device_events(by_check) + [(m['time'], 'removed', '') for m in removed]):
        if what == 'add':
            present.add(port)
        elif what == 'gone':
            present.discard(port)
        elif present:
            early.append(f'{time:%H:%M:%S} with {", ".join(sorted(present))}')
            present.clear()
    observed = f"{len(removed)} removals (Apple's lines)" + (
        f'; removed under a device: {"; ".join(early)}' if early else ''
    )
    return row('usb.port.detach-order', FAIL if early else PASS, observed, rule)


def detach_by_our_lines(devices: list[tuple], transports: list[dict], rule: str) -> Result:
    events = devices + [(m['time'], 'transport', m) for m in transports]
    present, kept, early, pairs, removals = set(), set(), [], 0, 0
    for time, what, item in sorted(events, key=lambda e: e[0]):
        if what == 'add':
            present.add(item)
        elif what == 'gone':
            present.discard(item)
        elif item['role'] == 'host':
            index = item['index']
            if item['what'] == 'added':
                kept.discard(index)
            elif item['what'] != 'removed':
                kept.add(index)
            else:
                removals += 1
                pairs += index in kept
                if present and index not in kept and re.fullmatch(REMOVED_IN_USE, item['why']):
                    early.append(f'{time:%H:%M:%S} USB {item["usb"]} ({item["why"]}) with {", ".join(sorted(present))}')
                kept.discard(index)
    if not removals:
        return row('usb.port.detach-order', NO_DATA, 'no host transport removed', rule)
    observed = f'{removals} host transport removals, {pairs} kept until the device went'
    observed += f'; removed under a device: {"; ".join(early)}' if early else ''
    return row('usb.port.detach-order', FAIL if early else PASS, observed, rule)


def controller_row(by_check: dict[str, Result], end: datetime | None) -> Result:
    """Return whether the controller reached On after each host transport went in (10 s)."""
    rule = 'On (3) within 10 s of each host transport'
    power = matches(by_check, 'usb.ctrl.power')
    adds = matches(by_check, 'usb.port.transport-host')
    if not power or not adds:
        missing = 'controller power-state line' if not power else 'host transport line'
        return row('usb.ctrl.reached-on', NO_DATA, f'no {missing} (development builds)', rule)
    window = timedelta(seconds=CONTROLLER_S)
    stuck, short, pending = [], [], 0
    for add in adds:
        before = [int(p['after']) for p in power if p['time'] <= add['time']]
        state = before[-1] if before else 0
        after = [p for p in power if add['time'] < p['time'] <= add['time'] + window]
        if state == ON or any(int(p['after']) == ON for p in after):
            continue
        if end is not None and add['time'] + window > end:
            pending += 1
        elif state == 0 and not after:
            stuck.append(f'{add["time"]:%H:%M:%S}')
        else:
            short.append(f'{add["time"]:%H:%M:%S} ({" > ".join([str(state)] + [p["after"] for p in after])})')
    observed = f'{len(adds)} host transports, {len(power)} power changes'
    if stuck:
        observed = (
            f'a host transport was added and the controller never left power state 0 within 10 s: {" ".join(stuck)}'
        )
    if short:
        observed += f'; did not reach On within 10 s: {"; ".join(short)}'
    if pending and not (stuck or short):
        return row('usb.ctrl.reached-on', NO_DATA, f'{observed}; {pending} with the log ending inside their 10 s', rule)
    return row('usb.ctrl.reached-on', FAIL if stuck or short else PASS, observed, rule)


def superspeed_row(by_check: dict[str, Result]) -> Result:
    """Return whether each SuperSpeed reset was taken with PP off and before the USB 3 transport went in."""
    rule = 'requested > taken (PP off); USB 3 transport after taken'
    resets = matches(by_check, 'usb.ss.resets')
    usb3 = [m for m in matches(by_check, 'usb.port.transports') if m['what'] == 'added' and m['usb'] == '3']
    usb3 = [m for m in usb3 if m['role'] == 'host']
    if not resets and not usb3:
        return row('usb.ss.reset-order', NO_DATA, 'no SuperSpeed reset or USB 3 transport line', rule)
    problems = []
    sessions = matches(by_check, 'usb.host.role')
    ends = matches(by_check, 'usb.dev.back')
    for i, reset in enumerate(resets):
        later = resets[i + 1 :]
        if reset.get('what') == 'requested':
            answer = next((r for r in later if r.get('what') == 'taken' or r.get('deferred')), None)
            if answer is None or (answer['time'] - reset['time']).total_seconds() > SS_ANSWER_S:
                problems.append(f'{reset["time"]:%H:%M:%S} requested, no taken or deferred within 5 s')
        elif reset.get('deferred'):
            end = next((e['time'] for e in ends if e['time'] > reset['time']), None)
            if not any(r.get('what') == 'requested' and (end is None or r['time'] < end) for r in later):
                problems.append(f'{reset["time"]:%H:%M:%S} deferred and never requested again')
        elif reset.get('what') == 'taken' and int(reset['after'], 16) & PP:
            problems.append(f'{reset["time"]:%H:%M:%S} taken with PP still set (PORTSC {reset["after"]})')
    for add in usb3:
        start = max((s['time'] for s in sessions if s['time'] <= add['time']), default=None)
        taken = [
            r
            for r in resets
            if r.get('sspi') == 'taken' and r['time'] <= add['time'] and (start is None or r['time'] >= start)
        ]
        if not taken:
            problems.append(f'{add["time"]:%H:%M:%S} USB 3 transport added before RESET_SSPI was taken')
    counts = {
        k: sum(1 for r in resets if r.get('what') == k or r.get('deferred') == k)
        for k in ('requested', 'deferred', 'taken')
    }
    observed = ', '.join(f'{n} {k}' for k, n in counts.items()) + f', {len(usb3)} USB 3 transports'
    return row('usb.ss.reset-order', FAIL if problems else PASS, observed + ''.join(f'; {p}' for p in problems), rule)


def attempts(by_check: dict[str, Result]) -> list[dict]:
    """Return each enumeration attempt in time order: its port location, its stale reads and how it ended."""
    ends = [(m, 'enumerated') for m in matches(by_check, 'usb.enum.devices')]
    ends += [(m, 'failed') for m in matches(by_check, 'usb.enum.failed') if m['what'] == 'enumeration failed']
    events = [(m['time'], 0, m, '') for m in matches(by_check, 'usb.enum.stale-lines')]
    events += [(m['time'], 1, m, outcome) for m, outcome in ends]
    found, reads, last = [], {}, ''
    for time, _, m, outcome in sorted(events, key=lambda e: e[:2]):
        loc = m.get('loc') or last
        if not outcome:
            reads.setdefault(loc, []).append(m)
            last = loc
        else:
            found.append(
                {'time': time, 'loc': loc, 'stale': reads.pop(loc, []), 'outcome': outcome, 'ids': m.get('ids')}
            )
    found += [
        {'time': ms[0]['time'], 'loc': loc, 'stale': ms, 'outcome': 'unfinished', 'ids': None}
        for loc, ms in reads.items()
    ]
    return sorted(found, key=lambda a: a['time'])


def stale_kind(m: dict) -> str:
    if 'fragment' in m:
        return 'device descriptor fragment invalid'
    if 'mps' in m:
        return 'endpoint 0 wMaxPacketSize 0x0000'
    return f'type 0x{m["want"]} read as type 0x{m["got"]}'


def device_text(start: datetime | None, tries: list[dict]) -> str:
    """Return one device's attempts in one host session (the attempts on one port location)."""
    stale = [a for a in tries if a['stale']]
    kinds = Counter(stale_kind(m) for a in stale for m in a['stale'])
    listed = ', '.join(kind + (f' x{n}' if n > 1 else '') for kind, n in kinds.items())
    done = [a for a in tries if a['outcome'] == 'enumerated']
    device = done[-1]['ids'] if done else f'an unidentified device at {tries[0]["loc"] or "an unknown port"}'
    text = f'{start:%H:%M:%S} ' if start else ''
    text += f'{device}: {len(stale)} of {len(tries)} attempts stale ({listed}), '
    if not done:
        return text + 'none enumerated'
    which = f'attempt {tries.index(done[-1]) + 1} enumerated it'
    return text + (f'{which}, reads in that attempt stale too' if done[-1]['stale'] else f'{which} and read correctly')


def stale_row(by_check: dict[str, Result]) -> Result:
    """Return the enumeration attempts that read back stale or zero control data, by host session and device.

    A device is the attempts on one port location within one host session.
    """
    rule = 'no descriptor read hands back bytes the device did not send'
    found = attempts(by_check)
    if not found:
        return row('usb.enum.stale-data', NO_DATA, 'no enumeration attempt', rule)
    starts = [m['time'] for m in matches(by_check, 'usb.host.role')]
    devices: dict[tuple[datetime | None, str], list[dict]] = {}
    for attempt in found:
        start = max((t for t in starts if t <= attempt['time']), default=None)
        devices.setdefault((start, attempt['loc']), []).append(attempt)
    sessions = {start for start, _ in devices}
    lines = [m for a in found for m in a['stale']]
    stale = sum(bool(a['stale']) for a in found)
    observed = f'{stale} of {len(found)} enumeration attempts in {len(sessions)} host sessions read stale or zero data'
    if not lines:
        return row('usb.enum.stale-data', PASS, observed, rule)
    parts = [device_text(start, tries) for (start, _), tries in devices.items() if any(a['stale'] for a in tries)]
    result = row('usb.enum.stale-data', FAIL, f'{observed} ({len(lines)} lines): ' + '; '.join(parts), rule)
    result.matches = lines
    return result


def wake_row(by_check: dict[str, Result]) -> Result:
    """Return, for each wake, the controller's exception codes and recoveries within 10 s of it."""
    wakes = matches(by_check, 'usb.sleep.wake')
    if not wakes:
        return row('usb.sleep.recoveries', NO_DATA, 'no wake line')

    def near(name: str, wake: dict) -> list[dict]:
        return [m for m in matches(by_check, name) if abs((m['time'] - wake['time']).total_seconds()) <= WAKE_S]

    parts = []
    for wake in wakes:
        codes = ', '.join(f'{m["code"]} {m["name"]}' for m in near('usb.ctrl.exceptions', wake)) or 'none'
        recoveries = len(near('usb.ctrl.recovery', wake))
        parts.append(f'wake {wake["wake"]} {wake["time"]:%H:%M:%S}: {codes}; {recoveries} recoveries')
    return row('usb.sleep.recoveries', INFO, '; '.join(parts))


def sequence_rows(by_check: dict[str, Result], log_end: datetime | None) -> list[Result]:
    return [
        detach_row(by_check),
        controller_row(by_check, log_end),
        superspeed_row(by_check),
        stale_row(by_check),
        driver_row(by_check, log_end),
        wake_row(by_check),
    ]
