"""Each configured device's class driver, step by step from its configuration.

A device configured as a hub is the hub driver's (usb.class.hub). Any other takes a kernel class driver (storage shows
an identified disk; the kernel HID driver is iOS 17's, and iOS 27's only as the fallback) or a DriverKit dext: the
kernel asks for the dext's server (`waiting for server`), the server checks in (`server launched`), the dext's service
starts (`::start(...) ok`, the one line a server already running prints too) and, for HID, IOHIDFamily opens the HID
device. The log never names an interface's class, so a device with none of these fails at the first step missing,
however long it stayed attached.
"""

import re
from collections.abc import Callable
from datetime import datetime, timedelta

from tools.devicerun.rules import FAIL, FALLBACK, NO_DATA, PASS, Check, Result, clock
from tools.devicerun.usb_class import HID_DEXT_S, HID_FALLBACK_S

OWNER = 'usb-host'
STEP_S = 5.0
DEXT_S = 4 * STEP_S
DISK_S = 10.0
KERNEL_HID = 'IOUSBHostHIDDevice'
HUB = re.compile(r'AppleUSB[23]0Hub$')
RULE = (
    'a configured device: its dext requested, launched, started and (HID) opened, each within 5 s of the last, or a '
    'kernel class driver'
)
SEVERITY = [FAIL, FALLBACK, PASS, NO_DATA]
Match = Callable[[dict], bool]


def lines(by_check: dict[str, Result], name: str) -> list[dict]:
    return sorted(by_check[name].matches if name in by_check else [], key=lambda m: m['time'])


class Plug:
    """One configured device, its teardown, and the driver lines it claims from those no other device has."""

    def __init__(self, configured: dict, by_check: dict[str, Result], taken: set[int], log_end: datetime | None):
        self.by_check, self.taken, self.log_end = by_check, taken, log_end
        self.time, loc = configured['time'], configured['loc']
        gone = [m for m in lines(by_check, 'usb.enum.destroyed') if m.get('loc') == loc and m['time'] > self.time]
        self.gone = gone[0] if gone else None
        enumerated = [m for m in lines(by_check, 'usb.enum.devices') if m['loc'] == loc and m['time'] <= self.time]
        self.name = (f'{enumerated[-1]["ids"]} ' if enumerated else '') + f'({configured["product"]})'

    def since(self, m: dict) -> str:
        return f'+{(m["time"] - self.time).total_seconds():.3f} s'

    def cut(self, start: datetime, seconds: float) -> bool:
        """Whether the log ended inside this window with the device still attached."""
        return not self.gone and self.log_end is not None and start + timedelta(seconds=seconds) > self.log_end

    def find(self, names: tuple[str, ...], start: datetime, seconds: float, same: Match | None = None) -> dict | None:
        """Return, and claim, the earliest unclaimed line of these checks in the window (which its teardown ends)."""
        stop = start + timedelta(seconds=seconds)
        stop = min(stop, self.gone['time']) if self.gone else stop
        found = [
            (m['time'], name, m)
            for name in names
            for m in lines(self.by_check, name)
            if start <= m['time'] <= stop and id(m) not in self.taken and (same is None or same(m))
        ]
        if not found:
            return None
        _, name, m = min(found, key=lambda f: f[0])
        self.taken.add(id(m))
        return {**m, 'check': name}

    def end_text(self) -> str:
        return f'its teardown at {self.since(self.gone)} ({self.gone["why"]})' if self.gone else "the log's end"

    def attached(self) -> str:
        attach = [m['time'] for m in lines(self.by_check, 'usb.host.role') if m['time'] <= self.time]
        detach = [m['time'] for m in lines(self.by_check, 'usb.dev.back') if m['time'] >= self.time]
        if not attach or not detach:
            return ''
        return f'; attach to detach {(detach[0] - attach[-1]).total_seconds():.1f} s'


def never_started(plug: Plug, step: str, names: tuple[str, ...], after: dict | None) -> tuple[str, str]:
    """Return the FAIL for the first step missing, with when (if ever) its line came."""
    start = after['time'] if after else plug.time
    text = f'driver never started: {plug.name} configured {clock(plug.time)}, no {step} within {STEP_S:.0f} s of '
    text += f'{after["what"]} ({plug.since(after)})' if after else 'its configuration'
    late = plug.find(names, start, 1e9)
    text += f', first at {plug.since(late)}' if late else f', none by {plug.end_text()}'
    if after is None:
        mig = lines(plug.by_check, 'usb.class.driverkitd')
        mig = [m for m in mig if timedelta(0) <= m['time'] - plug.time <= timedelta(seconds=STEP_S)]
        text += f'; {len(mig)} driverkitd MIG messages in its first {STEP_S:.0f} s'
    return FAIL, text + plug.attached()


def kernel_device(m: dict) -> bool:
    return m['device'] == KERNEL_HID


def kernel_hid(plug: Plug, failed_launches: int, ios27: bool) -> tuple[str, str] | None:
    """Return the kernel HID driver's verdict when it opened this device: iOS 27's fallback, iOS 17's driver."""
    opened = plug.find(('usb.class.hid-open',), plug.time, HID_FALLBACK_S if ios27 else STEP_S, kernel_device)
    if opened is None:
        return None
    if not ios27:
        return PASS, f'{plug.name}: kernel HID driver opened it {plug.since(opened)}'
    how = f'after {failed_launches} failed dext launches' if failed_launches else 'with no dext asked for'
    return FALLBACK, f'{plug.name}: kernel HID driver opened it {plug.since(opened)} {how}'


def kernel_class(plug: Plug, ios27: bool) -> tuple[str, str] | None:
    """Return the verdict of a kernel class driver that took the device with no dext asked for, if one did."""
    kernel = kernel_hid(plug, 0, ios27)
    if kernel:
        return kernel
    started = plug.find(('usb.class.hid-kernel',), plug.time, STEP_S)
    if started:
        text = f'kernel HID driver started {plug.since(started)} and nothing opened it within {STEP_S:.0f} s'
        return FAIL, f'driver never started: {plug.name}: {text}' + plug.attached()
    disk = plug.find(('usb.class.storage',), plug.time, DISK_S)
    if disk:
        return PASS, f'{plug.name}: storage, {disk["disk"]} identified {plug.since(disk)}'
    opened = plug.find(('usb.class.hid-open',), plug.time, DEXT_S)
    if opened:
        how = f'{opened["device"]} opened {plug.since(opened)}'
        return PASS, f'{plug.name}: HID device {how}, no DriverKit line before it (dext steps unseen)'
    return None


def unasked(plug: Plug, deferred: bool, ios27: bool) -> tuple[str, str]:
    """Return the verdict of a device no dext was asked for: a kernel class driver, or no driver at all."""
    wait_s = HID_DEXT_S if deferred else STEP_S
    kernel = kernel_class(plug, ios27)
    if kernel:
        return kernel
    if plug.cut(plug.time, max(wait_s, DISK_S)):
        return NO_DATA, f'{plug.name}: the log ends inside its first {max(wait_s, DISK_S):.0f} s'
    if deferred and plug.gone:
        deferral = 'matching deferred until the IOKit daemon is up'
        return NO_DATA, f'{plug.name}: {deferral}, no dext asked for by {plug.end_text()}'
    names = ('usb.dext.wait', 'usb.dext.start', 'usb.class.storage')
    return never_started(plug, 'dext request, dext start or kernel class driver line', names, None)


def refused(plug: Plug, start: datetime, stop: datetime) -> int:
    """Return how many dext processes in a window the kernel refused their dyld shared cache (shared_region)."""
    return sum(start <= m['time'] <= stop for m in lines(plug.by_check, 'usb.dext.shared-region'))


def launches_died(plug: Plug, asked: dict, start: datetime, stop: datetime, same: bool) -> list[dict]:
    """Return each launch of the asked server (`same` false: of any other HID dext server) given up on, in order.

    The services waiting on one server each print the failure; a launch is one retries count of one server.
    """
    died: dict[tuple[str, str], dict] = {}
    for m in lines(plug.by_check, 'usb.class.hid-dext-failed'):
        if start <= m['time'] <= stop and (m['tag'] == asked['tag']) == same:
            died.setdefault((m['tag'], m['retries']), m)
    return sorted(died.values(), key=lambda m: m['time'])


def died_text(plug: Plug, what: str, died: list[dict], start: datetime, stop: datetime) -> str:
    """Return a dext's launches that died, why, and when the last did."""
    count = refused(plug, start, stop)
    reason = (
        f"the kernel refused {count} dext processes their dyld cache: not on process' root volume nor preboot volume"
    )
    text = f'{len(died)} launches of {what} died ({reason if count else "no reason in the log"})'
    return text + (f', the last {plug.since(died[-1])}' if died else '')


def fallback(plug: Plug, asked: dict) -> tuple[str, str] | None:
    """Return the verdict of a dext whose launches died: the kernel driver's fallback, or how far it had got.

    None when no launch died: not the fallback.
    """
    end = plug.gone['time'] if plug.gone else plug.log_end or asked['time'] + timedelta(seconds=HID_FALLBACK_S)
    if not launches_died(plug, asked, asked['time'], end, True) and not refused(plug, asked['time'], end):
        return None
    started = plug.find(('usb.class.hid-kernel',), asked['time'], HID_FALLBACK_S)
    opened = plug.find(('usb.class.hid-open',), started['time'], HID_FALLBACK_S, kernel_device) if started else None
    stop = opened['time'] if opened else end
    first_stop = started['time'] if started else stop
    died = launches_died(plug, asked, asked['time'], first_stop, True)
    steps = [f'dext requested {plug.since(asked)}', died_text(plug, asked['server'], died, asked['time'], first_stop)]
    if started:
        steps.append(f'kernel HID driver started {plug.since(started)}')
        other = launches_died(plug, asked, started['time'], stop, False)
        if other or refused(plug, started['time'], stop):
            steps.append(died_text(plug, "its event driver's dext", other, started['time'], stop))
    text = ', then '.join(steps)
    if opened:
        return FALLBACK, f'{plug.name}: {text}, then IOHIDEventDriver opened it {plug.since(opened)}'
    if plug.gone:
        progress = f'{text}; the kernel HID driver had not ' + ('opened it' if started else 'started')
        return NO_DATA, f'{plug.name}: on the kernel fallback at {plug.end_text()}: {progress}' + plug.attached()
    if plug.cut(asked['time'], HID_FALLBACK_S):
        return NO_DATA, f'{plug.name}: the log ends on the kernel fallback: {text}'
    return FAIL, f'driver never started: {plug.name}: the kernel fallback stalled: {text}'


def launch(plug: Plug, asked: dict, ios27: bool) -> list[dict] | tuple[str, str]:
    """Return the requested server's launch and its service's start as steps, or the verdict when one is missing."""

    def same(m: dict) -> bool:
        return m['service'] == asked['service']

    launched = plug.find(('usb.dext.launched',), asked['time'], STEP_S, same)
    if launched is None:
        kernel = fallback(plug, asked) or kernel_hid(plug, 0, ios27)
        if kernel:
            return kernel
        if plug.cut(asked['time'], STEP_S):
            return NO_DATA, f'{plug.name}: the log ends inside its dext launch'
        return never_started(plug, 'dext launch', ('usb.dext.launched',), asked)
    launched = {**launched, 'what': 'its launch'}
    started = plug.find(('usb.dext.start',), launched['time'], STEP_S)
    if started is None:
        return never_started(plug, 'dext start', ('usb.dext.start',), launched)
    return [launched, {**started, 'what': 'its start'}]


def dext_steps(plug: Plug, asked: dict, ios27: bool) -> tuple[str, str]:
    """Return the verdict of a device whose dext was asked for (or, its server already running, started at once)."""
    steps = [asked]
    if asked['check'] == 'usb.dext.wait':
        more = launch(plug, asked, ios27)
        if isinstance(more, tuple):
            return more
        steps += more
    last = steps[-1]
    if last.get('result') == 'fail':
        return FAIL, f'driver never started: {plug.name}: {last["message"]} {plug.since(last)}'
    if 'HID' in asked.get('server', '') + last['service']:
        opened = plug.find(('usb.class.hid-open',), last['time'], STEP_S)
        if opened is None and plug.cut(last['time'], STEP_S):
            return NO_DATA, f'{plug.name}: the log ends before its HID device could open'
        if opened is None:
            return never_started(plug, 'HID device opened', ('usb.class.hid-open',), last)
        steps.append({**opened, 'what': 'its HID device opened'})
    return PASS, f'{plug.name}: ' + ', '.join(f'{s["what"][4:]} {plug.since(s)}' for s in steps)


def judge(plug: Plug, ios27: bool) -> tuple[str, str]:
    deferred = plug.find(('usb.class.hid-deferred',), plug.time, STEP_S)
    asked = plug.find(('usb.dext.wait', 'usb.dext.start'), plug.time, HID_DEXT_S if deferred else STEP_S)
    if asked is None:
        return unasked(plug, deferred is not None, ios27)
    what = 'its dext request' if asked['check'] == 'usb.dext.wait' else 'its dext start'
    return dext_steps(plug, {**asked, 'what': what}, ios27)


def driver_row(by_check: dict[str, Result], log_end: datetime | None) -> Result:
    """Return how each configured device's class driver came up, the worst verdict first."""
    devices = [m for m in lines(by_check, 'usb.enum.configured') if not HUB.match(m['driver'])]
    if not devices:
        return Result(Check('usb.class.driver', OWNER, '', RULE), NO_DATA, 'no device configured but hubs')
    ios27 = any(m.get('darwin') == '25' for m in lines(by_check, 'usb.kernel'))
    taken: set[int] = set()
    # Latest first: a driver line goes to the nearest configuration before it, as a re-enumeration's would.
    verdicts = [(judge(Plug(m, by_check, taken, log_end), ios27), m) for m in reversed(devices)][::-1]
    worst = min((verdict for (verdict, _), _ in verdicts), key=SEVERITY.index)
    ordered = sorted(verdicts, key=lambda v: SEVERITY.index(v[0][0]))
    result = Result(Check('usb.class.driver', OWNER, '', RULE), worst, '; '.join(text for (_, text), _ in ordered))
    result.matches = [m for (verdict, _), m in verdicts if verdict == worst]
    return result
