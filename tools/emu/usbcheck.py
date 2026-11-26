"""check.py's `usb` milestone: the devices on the emulated USB-C port, enumerated by Apple's host stack.

It is reached when every device plugged in with --usb has Apple's enumeration line naming its product
(AppleUSBHostPort::enumerateDeviceComplete). It fails at the first line that ends host mode or gives a device up: the
role thread's "host mode off", a connector root port that did not start or a transport the USB-C port refused, a
device Apple could not create or identify, a port it stopped enumerating.

The devices' pcaps (usbdev.py) then say what each saw: every device must have been asked for its device descriptor and
given a configuration. The class requests that followed and the interrupter targets the xHCI traced are listed. A
register access the XUSB model refused (its window unpowered, unclocked or not yet decoded) and an SMMU fault on the
host's DMA fail it too, since both read as zeros to the driver.
"""

import re
import struct
from collections import Counter
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from pathlib import Path

from tools.emu import usbdev

FAILURES = re.compile(
    r'SwitchUSB: host mode off'
    r'|SwitchPD: BM92T36 not usable'
    r'|SwitchUSB: USB-C root port \d+ did not start'
    r'|SwitchUSB: USB-C port refused'
    r'|createDevice: failed to create device'
    r'|device identification blocked'
    r'|persistent enumeration failures'
)
ENUMERATED = 'enumerated 0x'
TARGET = re.compile(r'usb_xhci_(?:slot_)?intr_target (.*)$')
REFUSED = re.compile(r'tegra\.xusb: .*|tegra\.xusb_host\.smmu: SMMU fault .*')

PCAP_HEADER = 24
RECORD_HEADER = 16
USBMON = struct.Struct('<QBBBBHbbqiiIIQiiII')  # QEMU's pcap.c usbmon_packet, 64 bytes
CONTROL, BULK, INTERRUPT = 2, 3, 1
GET_DESCRIPTOR, SET_CONFIGURATION = 6, 9


@dataclass(frozen=True)
class Urb:
    event: str  # 'S' submitted, 'C' completed
    transfer: int  # usbmon xfer_type: 0 isochronous, 1 interrupt, 2 control, 3 bulk
    endpoint: int  # with the direction bit (0x80 IN)
    status: int
    length: int
    setup: bytes
    data: bytes


def urbs(path: Path) -> list[Urb]:
    """Return the records of one of QEMU's USB pcaps (LINKTYPE_USB_LINUX_MMAPPED)."""
    raw = path.read_bytes() if path.exists() else b''
    found, offset = [], PCAP_HEADER
    while offset + RECORD_HEADER <= len(raw):
        included = struct.unpack_from('<I', raw, offset + 8)[0]
        record = raw[offset + RECORD_HEADER : offset + RECORD_HEADER + included]
        offset += RECORD_HEADER + included
        if len(record) < USBMON.size:
            break
        fields = USBMON.unpack_from(record)
        event, transfer, endpoint, setup_flag, status, length = (
            chr(fields[1]),
            fields[2],
            fields[3],
            fields[6],
            fields[10],
            fields[11],
        )
        setup = record[40:48] if setup_flag == 0 else b''
        found.append(Urb(event, transfer, endpoint, status, length, setup, record[USBMON.size :]))
    return found


def requests(found: Sequence[Urb]) -> list[str]:
    """Name the control requests submitted, in order (bmRequestType, bRequest, wValue, wIndex)."""
    named = []
    for urb in found:
        if urb.event != 'S' or urb.transfer != CONTROL or len(urb.setup) != 8:
            continue
        kind, request, value, index, _ = struct.unpack('<BBHHH', urb.setup)
        named.append(f'{kind:02x} {request:02x} {value:04x} {index:04x}')
    return named


def reached(products: Sequence[str]) -> Callable[[Sequence[str]], str | None]:
    """Return a finder for the line that completes the set: every product enumerated."""

    def find(log: Sequence[str]) -> str | None:
        last = None
        for product in products:
            line = next(
                (line for line in log if ENUMERATED in line and f'({product} /' in line),
                None,
            )
            if line is None:
                return None
            last = line
        return last

    return find


def failure(log: Sequence[str]) -> str | None:
    return next((line for line in log if FAILURES.search(line)), None)


def guest_lines(run: Path) -> list[str]:
    guest_log = run / 'qemu-guest.log'
    return guest_log.read_text(errors='replace').splitlines() if guest_log.exists() else []


def interrupter_targets(lines: list[str]) -> Counter:
    """Count the xHCI's traces of non-zero interrupter targets (patch 0045) by what they name."""
    return Counter(match.group(1) for line in lines if (match := TARGET.search(line)))


def after_run(run: Path, names: Sequence[str]) -> tuple[list[str], list[str]]:
    """Return what the devices' pcaps hold against the milestone, and what they and the trace show."""
    problems, notes = [], []
    for index, name in enumerate(names):
        found = urbs(run / usbdev.pcap(index, name))
        named = requests(found)
        if not any(r.startswith(f'80 {GET_DESCRIPTOR:02x} 01') for r in named):
            problems.append(f'{name}: never asked for its device descriptor')
        if not any(r.startswith(f'00 {SET_CONFIGURATION:02x}') for r in named):
            problems.append(f'{name}: never given a configuration')
        moved = Counter(
            ('bulk' if u.transfer == BULK else 'interrupt', u.endpoint)
            for u in found
            if u.event == 'C' and u.transfer in (BULK, INTERRUPT) and u.length
        )
        notes.append(
            f'{name}: control {", ".join(named) or "none"}; '
            + (', '.join(f'{kind} {ep:#04x} x{n}' for (kind, ep), n in sorted(moved.items())) or 'no data moved')
        )
    guest = guest_lines(run)
    problems += [match.group(0) for line in guest if (match := REFUSED.search(line))][:4]
    targets = interrupter_targets(guest)
    notes += [f'interrupter target: {what} x{n}' for what, n in sorted(targets.items())] or [
        'interrupter targets: none other than 0'
    ]
    return problems, notes
