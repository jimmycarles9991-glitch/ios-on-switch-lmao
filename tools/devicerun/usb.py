"""The `usb` group and its summary rows: one row per layer of the host path and device mode, and the first failure.

Also the order rows (usb_sequence.py), the XUSB clients' DMA faults and the DMA probe (usb_dma.py), the pad fields and
uncreated ports from the development role line, the XUDC stats against their baseline, and the USB panics in the crash
reports. A layer fails when any of its failure checks fails (its
row names the earliest failing line), passes when its evidence does (all of it for the IOPort layer), and has no data
otherwise. The IOPort, transport and controller layers rest on the driver's development lines. The first failure is the
host path's; SuperSpeed's and device mode's failures follow it among the later ones.
"""

import json
import re
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

from tools.devicerun.rules import FAIL, FALLBACK, INFO, NA, NO_DATA, PASS, REPORT, Check, Result
from tools.devicerun.usb_class import USB_CLASS
from tools.devicerun.usb_device import USB_DEVICE, no_partner_row, stats_row
from tools.devicerun.usb_dma import USB_DMA, fault_row
from tools.devicerun.usb_host import OWNER, USB_HOST
from tools.devicerun.usb_probe import probe_row, window_row
from tools.devicerun.usb_sequence import sequence_rows
from tools.devicerun.usb_watch import watch_rows

USB = USB_HOST + USB_CLASS + USB_DEVICE + USB_DMA
LABEL = 'U'

STEPS = [
    'USB boot, on a development build: unlock first and keep it unlocked. Write down the wall-clock time of every plug',
    'and pull, what the screen showed (an accessory prompt, Files, typed text) and how long each device took.',
    '1 A USB stick: wait for Files to show it, then pull it.',
    '2 A USB keyboard: type into Notes, then pull it. On iOS 27 it waits on its DriverKit dext first.',
    '  An iOS 27 image older than the pretranslated DriverKit cache: no dext; kernel HID after 120 s, up to 3 tries.',
    '3 A hub with the keyboard and the stick both on it: type, open the stick in Files; pull the hub.',
    '4 Back to the Mac on the same cable: Finder or Xcode sees the iPad. Leave it connected.',
    '5 Pull, on the Mac (the age limit, in seconds, must reach back to the boot):',
    '  L="$HOME/Library/Application Support/switch-ios/device-logs/U"; mkdir -p "$L" && cd "$L"',
    '  idevicesyslog -u <udid> archive archive.tar --age-limit <seconds since boot> && tar -xf archive.tar',
    '  mv <the extracted .logarchive folder> u.logarchive && idevicecrashreport -u <udid> -k -e crashes',
    '6 Check, in the same shell, after cd to the repository root:',
    '  python3 -m tools.devicerun.devicerun "$L/u.logarchive" --boot-label U --crashes "$L/crashes" \\',
    '    --tsv "$L/checks.tsv"',
    '  Read usb.host.first-failure, then the usb-layers rows; FALLBACK on usb.class.driver is named, not a failure.',
]


@dataclass(frozen=True)
class Layer:
    name: str
    title: str
    evidence: tuple[str, ...]
    failures: tuple[str, ...]
    all_evidence: bool = False


LAYERS = [
    Layer(
        'pd',
        'PD attach, classify and VBUS',
        ('usb.pd.sourcing',),
        (
            'usb.pd.unusable',
            'usb.pd.classified',
            'usb.pd.failed',
            'usb.pd.sys-reset',
            'usb.pd.alt-failed',
            'usb.pd.ufp-write',
            'usb.pd.settings',
        ),
    ),
    Layer(
        'bringup',
        'firmware and controller bring-up',
        ('usb.host.up', 'usb.host.role'),
        (
            'usb.host.mode-off',
            'usb.host.silent',
            'usb.host.not-loaded',
            'usb.host.mailbox',
            'usb.host.dma',
            'usb.smmu.fault',
        ),
    ),
    Layer('role', 'host role', ('usb.host.role',), ('usb.host.silent',)),
    Layer(
        'ioport',
        'USB-C IOPort, both root ports USB-C',
        ('usb.port.found-usbc', 'usb.port.created-usb2', 'usb.port.created-usb3'),
        (
            'usb.port.no-properties',
            'usb.port.no-ioport',
            'usb.port.found-wrong',
            'usb.port.created-other',
            'usb.port.created-usb2',
            'usb.port.created-usb3',
        ),
        all_evidence=True,
    ),
    Layer(
        'transport',
        'USB 2 transport with the host data role',
        ('usb.port.transport-host',),
        (
            'usb.port.transport-refused',
            'usb.port.usb2-removed',
            'usb.port.transport-duplicate',
            'usb.port.transport-terminated',
            'usb.port.transports-supported',
            'usb.port.identification-blocked',
            'usb.port.transport-host',
            'usb.port.detach-order',
            'usb.port.transport',
            'usb.port.data-role-host',
        ),
        all_evidence=True,
    ),
    Layer('port-power', "Apple's root port powering on", ('usb.port.powering-on',), ('usb.port.powering-on',)),
    Layer(
        'controller',
        'controller out of power state 0',
        ('usb.ctrl.reached-on', 'usb.ctrl.running', 'usb.enum.root'),
        (
            'usb.ctrl.reached-on',
            'usb.ctrl.deadline',
            'usb.ctrl.halted',
            'usb.ctrl.save-state',
            'usb.ctrl.exception',
            'usb.ctrl.running',
        ),
    ),
    Layer(
        'transfer-data',
        'control transfer data (the descriptors read back)',
        ('usb.enum.stale-data',),
        ('usb.enum.stale-data', 'usb.dma.unmap-unmatched', 'usb.dma.copyback-incomplete'),
    ),
    Layer(
        'enumeration',
        "enumeration (Apple's line)",
        ('usb.enum.root',),
        ('usb.enum.root', 'usb.enum.after-role', 'usb.enum.fold', 'usb.enum.failed', 'usb.iokit.bad-busy-count'),
    ),
    Layer(
        'configuration', 'configuration', ('usb.enum.configured',), ('usb.enum.configured', 'usb.enum.configured-each')
    ),
    Layer(
        'class',
        'class drivers (storage, HID, hub, dexts)',
        ('usb.class.storage', 'usb.class.driver', 'usb.class.hid-kernel', 'usb.class.hub'),
        (
            'usb.class.driver',
            'usb.class.hub',
            'usb.class.hub-port-disabled',
            'usb.dext.waits-answered',
            'usb.dext.failed',
        ),
    ),
    Layer(
        'superspeed',
        'SuperSpeed PHY preparation',
        ('usb.ss.lane-up',),
        (
            'usb.ss.lane-up',
            'usb.ss.mailbox',
            'usb.ss.reset-sspi',
            'usb.ss.refused',
            'usb.ss.reset-order',
            'usb.ss.uphy-uncalibrated',
            'usb.ss.off',
        ),
    ),
    Layer(
        'device',
        'device mode on the Mac, through the SMMU',
        ('usb.dev.connected',),
        (
            'usb.dev.off',
            'usb.port.device-role-powered',
            'usb.dev.connected',
            'usb.dev.no-partner',
            'usb.smmu.fault-device',
            'usb.dev.smmu-off',
            'usb.dev.dci-bits',
            'usb.dev.transfer-lost',
            'usb.dev.residue',
            'usb.dev.broken',
        ),
    ),
]
CHAIN = [layer.name for layer in LAYERS if layer.name not in {'superspeed', 'device'}]


def row(name: str, verdict: str, observed: str, rule: str = REPORT, source: str = '') -> Result:
    return Result(Check(name, OWNER, '', rule, source=source), verdict, observed)


def first_time(result: Result) -> float:
    return min((m['time'].timestamp() for m in result.matches if 'time' in m), default=float('inf'))


def proves(result: Result | None) -> bool:
    return result is not None and (result.verdict == PASS or (result.verdict == INFO and bool(result.matches)))


def layer_verdict(layer: Layer, by_check: dict[str, Result]) -> tuple[str, str]:
    failing = [by_check[n] for n in layer.failures if n in by_check and by_check[n].verdict == FAIL]
    if failing:
        first = min(failing, key=lambda r: (first_time(r), layer.failures.index(r.check.name)))
        lines = [m for m in first.matches if 'time' in m]
        if not lines:
            return FAIL, f'{first.check.name}: {first.observed} (no {first.check.literals()[0]!r} line)'
        line = min(lines, key=lambda m: m['time'])
        return FAIL, f'{first.check.name}: {first.observed}; first {line["time"]:%H:%M:%S} {line["message"][:120]}'
    evidence = [by_check[n] for n in layer.evidence if n in by_check and by_check[n].verdict != NA]
    fallback = [r for r in evidence if r.verdict == FALLBACK]
    if fallback:
        return FALLBACK, '; '.join(f'{r.check.name}: {r.observed}' for r in fallback)
    ok = [r for r in evidence if proves(r)]
    if evidence and (len(ok) == len(evidence) if layer.all_evidence else ok):
        return PASS, '; '.join(f'{r.check.name}: {r.observed}' for r in ok)
    missing = [r for r in evidence if not proves(r)] or evidence
    return NO_DATA, '; '.join(f'{r.check.name}: {r.observed}' for r in missing) or 'none of its checks ran'


def layer_rows(by_check: dict[str, Result]) -> list[Result]:
    """Return one row per layer, then the host path's first failing layer and the gaps before it."""
    rows, verdicts = [], {}
    for index, layer in enumerate(LAYERS, 1):
        verdict, observed = layer_verdict(layer, by_check)
        verdicts[layer.name] = verdict
        rows.append(row(f'usb.layer.{index:02d}-{layer.name}', verdict, f'{layer.title}: {observed}'))
    failed = [name for name in CHAIN if verdicts[name] == FAIL]
    gaps = [name for name in CHAIN if verdicts[name] == NO_DATA]
    off_chain = [layer.name for layer in LAYERS if layer.name not in CHAIN and verdicts[layer.name] == FAIL]
    if failed:
        later = f'; later failures: {", ".join(failed[1:] + off_chain)}' if failed[1:] + off_chain else ''
        before = [g for g in gaps if CHAIN.index(g) < CHAIN.index(failed[0])]
        observed = f'{failed[0]}{later}' + (f'; no data before it: {", ".join(before)}' if before else '')
        rows.append(row('usb.host.first-failure', FAIL, observed, 'no failing layer'))
    elif all(verdicts[name] == NO_DATA for name in CHAIN):
        rows.append(row('usb.host.first-failure', NO_DATA, 'no host-mode line at all', 'no failing layer'))
    else:
        degraded = [name for name in CHAIN if verdicts[name] == FALLBACK]
        observed = 'every layer passes' + (f'; no data: {", ".join(gaps)}' if gaps else '')
        observed += f'; through a fallback: {", ".join(degraded)}' if degraded else ''
        rows.append(row('usb.host.first-failure', PASS, observed, 'no failing layer'))
    return rows


PADS = re.compile(
    r'PADCTL PORT_CAP (?P<cap>0x[0-9a-f]+|0) .* OTG_PAD0 (?P<otg>0x[0-9a-f]+|0)/\S+ .* BIAS (?P<bias>0x[0-9a-f]+|0)'
)
PORTSC = re.compile(r'PORTSC((?: \d+:(?:0x[0-9a-f]+|0))+)')
UNCREATED = (2, 3, 4, 6, 7, 8, 9)


def pad_rows(reports: list[dict]) -> list[Result]:
    """Return the L4T pad values from the role line: BIAS 5:3 = 7 and 10:8 = 3, the fuse-set fields, PORT_CAP 3."""
    lines = [(m['role'], PADS.search(m['message'])) for m in reports]
    lines = [(role, p) for role, p in lines if p]
    if not lines:
        return [row('usb.pad.values', NO_DATA, 'no role line with the pad fields (development builds)')]
    role, last = lines[-1]
    bias, otg = int(last['bias'], 16), int(last['otg'], 16)
    hs_disconnect, field_10_8 = (bias >> 3) & 7, (bias >> 8) & 7
    device_caps = sorted({int(p['cap'], 16) & 3 for r, p in lines if r == 'device'})
    rows = [
        row(
            'usb.pad.bias-hs-disconnect',
            PASS if hs_disconnect == 7 else FAIL,
            f'BIAS {last["bias"]}: {hs_disconnect}',
            'BIAS 5:3 == 7',
        ),
        row(
            'usb.pad.bias-10-8',
            PASS if field_10_8 == 3 else FAIL,
            f'BIAS {last["bias"]}: {field_10_8}',
            'BIAS 10:8 == 3',
        ),
        row('usb.pad.squelch', INFO, f'BIAS 2:0 = {bias & 7} (the fuse squelch level), {role} role line'),
        row('usb.pad.otg-pad0', INFO, f'OTG_PAD0 5:0 = {otg & 0x3F} (fuse + 6)'),
    ]
    if device_caps:
        verdict = PASS if device_caps == [3] else FAIL
        rows.append(row('usb.pad.port-cap-device', verdict, f'PORT_CAP 1:0 in device role: {device_caps}', 'all == 3'))
    else:
        rows.append(row('usb.pad.port-cap-device', NO_DATA, 'no device-role line', 'all == 3'))
    return rows


def uncreated_row(reports: list[dict]) -> Result:
    """Return the seven connectorless ports' PORTSC from the second host session's role line (else the first)."""
    hosts = [m for m in reports if m['role'] == 'host' and PORTSC.search(m['message'])]
    if not hosts:
        return row('usb.info.uncreated-ports', NO_DATA, 'no host-role line with PORTSC (development builds)')
    chosen = hosts[1] if len(hosts) > 1 else hosts[0]
    values = dict(pair.split(':') for pair in PORTSC.search(chosen['message'])[1].split())
    listed = ' '.join(f'{p}:{values.get(str(p), "?")}' for p in UNCREATED)
    which = 'second host session' if len(hosts) > 1 else 'only host session'
    return row('usb.info.uncreated-ports', INFO, f'{which} ({chosen["time"]:%H:%M:%S}): {listed}')


def panic_row(crashes: str | None) -> Result:
    """Return the panics whose string a USB driver wrote (the log never holds a panic's text)."""
    if not crashes:
        return row('usb.panic', NO_DATA, 'no --crashes directory', 'count == 0')
    found = []
    for path in sorted(Path(crashes).rglob('*.ips')):
        text = path.read_text(errors='replace')
        for part in text.split('\n', 1)[1:]:
            try:
                panic = json.loads(part).get('panicString', '')
            except json.JSONDecodeError:
                panic = ''
            hit = re.search(r'(?:SwitchUSB|SwitchPD|SwitchXUDC): [^\n]{0,120}', panic)
            if hit:
                found.append(f'{path.name}: {hit[0]}')
    return row('usb.panic', FAIL if found else PASS, '; '.join(found[:3]) or 'no USB panic', 'count == 0')


def rows(by_check: dict[str, Result], crashes: str | None = None, log_end: datetime | None = None) -> list[Result]:
    """Return the group's summary rows (devicerun's 'usb-layers' group); `log_end` is the log's last line's time."""
    dma = [fault_row(by_check, 'host'), fault_row(by_check, 'device'), probe_row(by_check), window_row(by_check)]
    sequences = [*sequence_rows(by_check, log_end), no_partner_row(by_check), *dma]
    judged = {**by_check, **{r.check.name: r for r in sequences}}
    reports = by_check['usb.role.report'].matches if 'usb.role.report' in by_check else []
    stats = by_check.get('usb.dev.stats')
    summary = [*layer_rows(judged), *sequences, *watch_rows(by_check), *pad_rows(reports), uncreated_row(reports)]
    if stats is not None:
        summary.append(stats_row(stats))
    return [*summary, panic_row(crashes)]
