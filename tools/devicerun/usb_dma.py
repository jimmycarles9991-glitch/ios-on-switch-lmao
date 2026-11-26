"""DMA on the XUSB clients: the memory controller's fault reports, the mapper's own errors and the development probe.

The memory controller reports each error it captured with its client (MC_ERR_STATUS ERR_ID) and address: the XUSB host
reads as client 0x4a and writes as 0x4b, the device as 0x4c and 0x4d. A host fault explains stale transfer data, so its
row is a bring-up failure, ahead of the descriptor reads; a device fault is device mode's.

A client's run of errors prints its first four in full; when the run ends one run-over line gives the run's count and
why it ended: displaced (16 errors by other clients, or a ninth client's run), its swgroup's detach, or SC7's resume. A
build before the cause prints the line without it. A run of four or fewer ends without the line, so four full lines and
no run-over line is at least four errors.
"""

from tools.devicerun.rules import FAIL, NO_DATA, PASS, REPORT, Check, Result, clock

KERNEL = 'kernel'
OWNER = 'usb-host'
MC_ERRORS = 'drivers/TegraSMMU/TegraMCErrors.cpp, drivers/TegraSMMU/TegraMCErrors.h'
MAPPER = 'drivers/SwitchUSB/UsbDmaMapper.cpp'
PROBE = 'drivers/SwitchUSB/UsbDmaProbe.cpp'
FULL_LINES = 4
HOST_CLIENTS = ('0x4a', '0x4b')
DEVICE_CLIENTS = ('0x4c', '0x4d')
MC_ERROR = (
    r'^TegraSMMU: MC error (?:(?P<kinds>[A-Z_+]+): (?P<type>[^,]*), a (?P<direction>read|write) by (?P<name>\S+) '
    r'\([^)]*\) \(client (?P<client>0x4[a-d])\) at (?P<address>0x[0-9a-f]+|0);'
    r'|run over: (?P<errors>\d+) errors by (?P<run_name>\S+) \([^)]*\) \(client (?P<run_client>0x4[a-d])\), the first '
    r'(?P<printed>\d+) printed(?:, ended by (?P<cause>displaced|detach|SC7))?;'
    r'|interrupt \(INTID \d+\) could not be attached(?P<unreported>))'
)

USB_DMA = [
    Check(
        'usb.smmu.faults',
        OWNER,
        'TegraSMMU: MC error ',
        REPORT,
        process=KERNEL,
        regex=MC_ERROR,
        show=('name', 'client'),
        source=MC_ERRORS,
        note='the XUSB clients only; usb.smmu.fault (host) and usb.smmu.fault-device judge them',
    ),
    Check(
        'usb.dma.unmap-unmatched',
        OWNER,
        'SwitchUSB: DMA unmap of ',
        process=KERNEL,
        regex=r'unmap of (?P<address>\S+) \+(?P<length>\d+) \((?P<memory>[^)]*)\) matches no mapping',
        show=('memory',),
        failure='a DMA unmap matched no mapping: no cache maintenance or copy back done',
        source=MAPPER,
    ),
    Check(
        'usb.dma.copyback-incomplete',
        OWNER,
        'SwitchUSB: incomplete DMA bounce copy back',
        process=KERNEL,
        failure='the host DMA bounce copy did not return the entire buffer',
        source=MAPPER,
    ),
    Check(
        'usb.dma.host-window',
        OWNER,
        'SwitchUSB: host DMA through the SMMU',
        REPORT,
        process=KERNEL,
        regex=r'a (?P<pages>\d+)-page window',
        show=('pages',),
        source='drivers/SwitchUSB/UsbSmmuMapper.cpp',
    ),
    Check(
        'usb.dma.probe',
        OWNER,
        'SwitchUSB: DMA probe',
        REPORT,
        process=KERNEL,
        regex=r'^SwitchUSB: DMA probe(?: #\d+ (?P<kind>prepare|complete)\b)?',
        source=PROBE,
        note='development builds; usb.dma.probe-summary and usb.dma.window read it (usb_probe.py)',
    ),
]


def client_of(m: dict) -> str:
    return m.get('client') or m.get('run_client', '')


CAUSES = {
    'displaced': "displaced (16 errors by other clients, or a ninth client's run)",
    'SC7': "at SC7's resume",
    'detach': "at its swgroup's detach, which SwitchUSB never asks for on an XUSB group",
}


def run_end(over: dict) -> str:
    """Return a run that ended: its count, when, and why as the run-over line gives it."""
    cause = CAUSES[over['cause']] if 'cause' in over else 'cause not printed (a build before the cause)'
    return f'a run of {over["errors"]} bounded at {over["printed"]} lines, over at {clock(over["time"])} {cause}'


def client_text(lines: list[dict]) -> tuple[int, bool, str]:
    """Return one client's error count, whether it is a lower bound, and its text: count, first fault, its runs."""
    errors, full, runs = 0, [], []
    for m in lines:
        if 'errors' not in m:
            full.append(m)
            continue
        errors += int(m['errors']) + max(0, len(full) - int(m['printed']))
        runs.append(run_end(m))
        full = []
    errors += len(full)
    open_run = len(full) >= FULL_LINES
    if open_run:
        runs.append(f"a run bounded at {FULL_LINES} lines still open at the log's end")
    first = next((m for m in lines if 'errors' not in m), None)
    name = first['name'] if first else lines[0]['run_name']
    count = f'{"at least " if open_run else ""}{errors} error{"" if errors == 1 else "s"}'
    text = f'{name} ({client_of(lines[0])}): {count}'
    if first:
        text += f', the first a {first["direction"]} of {first["address"]} at {clock(first["time"])}, {first["type"]}'
    return errors, open_run, ', '.join([text, *runs])


def fault_row(by_check: dict[str, Result], side: str) -> Result:
    """Return the memory controller's faults on one side's XUSB clients: per client, how many, the first, its runs."""
    name, clients = ('usb.smmu.fault', HOST_CLIENTS) if side == 'host' else ('usb.smmu.fault-device', DEVICE_CLIENTS)
    check = Check(name, OWNER, 'TegraSMMU: MC error ', 'count == 0', source=MC_ERRORS)
    lines = by_check['usb.smmu.faults'].matches if 'usb.smmu.faults' in by_check else []
    if any('unreported' in m for m in lines):
        return Result(check, NO_DATA, 'the MC error interrupt could not be attached: faults go unreported this boot')
    faults = sorted((m for m in lines if client_of(m) in clients), key=lambda m: m['time'])
    if not faults:
        return Result(check, PASS, f'no XUSB {side} client fault reported')
    seen = list(dict.fromkeys(client_of(m) for m in faults))
    per_client = [client_text([m for m in faults if client_of(m) == c]) for c in seen]
    total = sum(errors for errors, _, _ in per_client)
    bound = 'at least ' if any(open_run for _, open_run, _ in per_client) else ''
    observed = f'{bound}{total} XUSB {side} faults: ' + '; '.join(text for _, _, text in per_client)
    return Result(check, FAIL, observed, faults)
