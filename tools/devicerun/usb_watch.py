"""Things a passing USB boot still shows that are not failures today, counted so a change in them is seen.

The SuperSpeed root port's power-off that the driver waits 700 ms for, the PD controller's aborted request and data
role swap with a Mac, endpoint transaction errors as a device is pulled, and the USB host objects still alive when
the controller powers off against the next session's start.
"""

from datetime import datetime, timedelta
from itertools import pairwise

from tools.devicerun.rules import FAIL, INFO, NO_DATA, REPORT, Check, Result, clock
from tools.devicerun.usb_probe import summaries

OWNER = 'usb-host'
DETACH_S = 1.0
OBJECTS = ('IOUSBHostDevice', 'IOUSBHostInterface')


def lines(by_check: dict[str, Result], name: str) -> list[dict]:
    return sorted(by_check[name].matches if name in by_check else [], key=lambda m: m['time'])


def row(name: str, verdict: str, observed: str, matches: list[dict] | None = None) -> Result:
    return Result(Check(name, OWNER, '', REPORT), verdict, observed, matches or [])


def session_of(time: datetime, starts: list[datetime]) -> datetime | None:
    return max((t for t in starts if t <= time), default=None)


def power_off_row(by_check: dict[str, Result]) -> Result:
    """Return, per host session, the SuperSpeed root port power-offs that were not done at the first look."""
    starts = [m['time'] for m in lines(by_check, 'usb.host.role')]
    if not starts:
        return row('usb.watch.ss-power-off', NO_DATA, 'no host session')
    late = [m for m in lines(by_check, 'usb.info.settle') if m['step'] == 'SuperSpeed root port power-off']
    parts = []
    for start in starts:
        mine = [m for m in late if session_of(m['time'], starts) == start]
        took = ', '.join(f'{m["outcome"]} after {m["bound"]} ms' for m in mine)
        parts.append(f'{clock(start)}: {len(mine)}' + (f' ({took})' if mine else ''))
    return row('usb.watch.ss-power-off', INFO, f'{len(late)} in {len(starts)} host sessions: ' + '; '.join(parts), late)


def pd_mac_row(by_check: dict[str, Result]) -> Result:
    """Return the PD request aborted at attach and the data role swap aborted, each with when."""
    aborted = lines(by_check, 'usb.pd.request-aborted')
    swaps = [m for m in lines(by_check, 'usb.pd.alt') if 'SEND_VDM as UFP was aborted or terminated' in m['message']]
    if not aborted and not swaps:
        return row('usb.watch.pd-mac', NO_DATA, 'neither line')
    when = [' '.join(clock(m['time']) for m in found) for found in (aborted, swaps)]
    observed = f'request aborted at attach x{len(aborted)} ({when[0]}); '
    observed += f'no data role swap, SEND_VDM as UFP aborted x{len(swaps)} ({when[1]})'
    return row('usb.watch.pd-mac', INFO, observed, aborted + swaps)


def transaction_errors_row(by_check: dict[str, Result]) -> Result:
    """Return the endpoint transaction errors: those as a device was torn down, and a FAIL for any while attached."""
    errors = lines(by_check, 'usb.enum.transaction-error')
    if not errors:
        return row('usb.watch.transaction-errors', NO_DATA, 'no endpoint transaction error')
    window = timedelta(seconds=DETACH_S)
    gone = [m['time'] for m in lines(by_check, 'usb.enum.destroyed')]
    teardowns: dict[datetime, list[str]] = {}
    attached = []
    for error in errors:
        teardown = next((g for g in reversed(gone) if timedelta(0) <= error['time'] - g <= window), None)
        if teardown is None:
            attached.append(error)
        else:
            teardowns.setdefault(teardown, []).append(error['endpoint'])
    at_detach = sum(len(endpoints) for endpoints in teardowns.values())
    observed = f'{at_detach} at {len(teardowns)} teardowns'
    if teardowns:
        observed += ' (' + '; '.join(f'{clock(t)}: {" ".join(e)}' for t, e in teardowns.items()) + ')'
    if not attached:
        return row('usb.watch.transaction-errors', INFO, observed, errors)
    listed = ' '.join(f'{clock(e["time"])} {e["endpoint"]}' for e in attached)
    return row('usb.watch.transaction-errors', FAIL, f'{len(attached)} while attached: {listed}; {observed}', attached)


def live_objects_row(by_check: dict[str, Result]) -> Result:
    """Return the USB host objects alive at the power-off after each session and at the next session's start."""
    found = summaries(lines(by_check, 'usb.dma.probe'))
    after_role = [b for a, b in pairwise(found) if a['when'] == 'device role']
    offs = [s for s in after_role if s['when'] == 'controller off' and s['live']]
    if not offs:
        return row('usb.watch.live-objects', NO_DATA, 'no probe summary at a power-off after a session')

    def counts(summary: dict) -> str:
        return ', '.join(f'{name} {summary["live"].get(name, "?")}' for name in OBJECTS)

    parts = []
    for off in offs:
        start = next((s for s in found if s['time'] > off['time'] and s['when'] == 'host session start'), None)
        text = f'controller off {clock(off["time"])}: {counts(off)}'
        text += f'; next session start {clock(start["time"])}: {counts(start)}' if start else '; no later session start'
        parts.append(text)
    return row('usb.watch.live-objects', INFO, ' | '.join(parts))


def watch_rows(by_check: dict[str, Result]) -> list[Result]:
    return [power_off_row(by_check), pd_mac_row(by_check), transaction_errors_row(by_check), live_objects_row(by_check)]
