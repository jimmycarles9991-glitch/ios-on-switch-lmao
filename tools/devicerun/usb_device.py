"""USB device mode on the same boot: the XUDC back on a Mac after the host sessions, its DMA through the SMMU.

The development stats line is summed against the 2026-11-13 baseline: CPU 3.52 % of one core at about 400 requests/s,
bounces above 4 GiB present.
"""

import re

from tools.devicerun.rules import FAIL, INFO, NO_DATA, PASS, REPORT, TIMELINE, Check, Result, clock, span_opener
from tools.devicerun.usb_pd import PORT_EMPTY

KERNEL = 'kernel'
OWNER = 'usb-xudc-offload'
USB = 'drivers/SwitchUSB'
OFFLOAD = '(usb-xudc-offload)'
BASELINE = '2026-11-13: CPU 3.52 % at about 400 requests/s, bounces above 4 GiB present'

STATS = re.compile(
    r'stats (?P<ms>\d+) ms: IN (?P<in_mbs>[\d.]+) MB/s \((?P<in_req>\d+) requests, (?P<in_kib>\d+) KiB bounced, '
    r'(?P<in_serial>\d+) serial\), OUT (?P<out_mbs>[\d.]+) MB/s \((?P<out_req>\d+) requests, (?P<out_kib>\d+) KiB '
    r'bounced, (?P<out_serial>\d+) serial\); (?P<irqs>\d+) interrupts/s, (?P<events>\d+) events/s; CPU '
    r'(?P<cpu>[\d.]+)% of one core \(filter, events, queueing\); bounced packets: (?P<breaks>\d+) breaks, '
    r'(?P<k64>\d+) 64K, (?P<high>\d+) above 4G, (?P<edges>\d+) cache-line edges'
)

USB_DEVICE = [
    Check(
        'usb.dev.superspeed',
        OWNER,
        'SwitchUSB: host connected (super speed)',
        REPORT,
        process=KERNEL,
        source=f'{USB}/SwitchXUDCEvents.cpp',
        note='actual SuperSpeed device connection to the external host',
    ),
    Check(
        'usb.dev.ready',
        OWNER,
        'SwitchUSB: device mode ready (',
        TIMELINE,
        process=KERNEL,
        source=f'{USB}/SwitchUSB.cpp',
    ),
    Check(
        'usb.dev.off',
        OWNER,
        'SwitchUSB: device mode off',
        process=KERNEL,
        regex=r'off(?: after sleep)?: (?P<why>.*)$',
        show=('why',),
        source=f'{USB}/SwitchUSB.cpp',
    ),
    Check(
        'usb.dev.pull-up',
        OWNER,
        'SwitchUSB: functions registered, pull-up on',
        REPORT,
        process=KERNEL,
        source=f'{USB}/SwitchXUDC.cpp',
    ),
    Check(
        'usb.dev.connected',
        OWNER,
        'SwitchUSB: host connected (',
        'count > 0',
        process=KERNEL,
        regex=r'\((?P<speed>\w+) speed\)',
        show=('speed',),
        needs='usb.dev.pull-up',
        outside=PORT_EMPTY,
        source=f'{USB}/SwitchXUDCEvents.cpp {OFFLOAD}',
        note='anywhere in the boot: a Mac at boot counts as much as one after the host sessions; none on an empty port',
    ),
    Check(
        'usb.dev.connects',
        OWNER,
        'SwitchUSB: host connected (',
        TIMELINE,
        process=KERNEL,
        source=f'{USB}/SwitchXUDCEvents.cpp {OFFLOAD}',
        note='every connect the XUDC reported; usb.dev.no-partner holds them against the PD lines',
    ),
    Check(
        'usb.dev.back-on-mac',
        OWNER,
        'SwitchUSB: host connected (',
        REPORT,
        process=KERNEL,
        regex=r'\((?P<speed>\w+) speed\)',
        show=('speed',),
        after='usb.dev.back',
        within=600.0,
        outside=PORT_EMPTY,
        source=f'{USB}/SwitchXUDCEvents.cpp {OFFLOAD}',
        note='within 10 minutes after a host session ended (step 4: back to the Mac); no data when it was not done',
    ),
    Check(
        'usb.dev.back',
        OWNER,
        'SwitchUSB: device role',
        TIMELINE,
        process=KERNEL,
        regex=r'^SwitchUSB: device role$',
        source=f'{USB}/SwitchUSB.cpp',
    ),
    Check(
        'usb.dev.smmu',
        OWNER,
        'SwitchUSB: device DMA through the SMMU, RAM at fixed IOVAs',
        TIMELINE,
        process=KERNEL,
        source=f'{USB}/SwitchXUDCSmmu.cpp {OFFLOAD}',
    ),
    Check(
        'usb.dev.smmu-off',
        OWNER,
        'SwitchUSB: device DMA by physical address',
        process=KERNEL,
        regex=r'SMMU off: (?P<why>[^)]*)',
        show=('why',),
        source=f'{USB}/SwitchXUDCSmmu.cpp {OFFLOAD}',
    ),
    Check(
        'usb.dev.dci-bits',
        OWNER,
        'SwitchUSB: DCI bits ',
        process=KERNEL,
        regex=r'DCI bits \S+: (?P<register>\S+) reads',
        show=('register',),
        source=f'{USB}/XudcHardware.cpp {OFFLOAD}',
    ),
    Check(
        'usb.dev.transfer-lost',
        OWNER,
        'SwitchUSB: XUDC transfer event lost',
        process=KERNEL,
        source=f'{USB}/SwitchXUDCData.cpp {OFFLOAD}',
    ),
    Check(
        'usb.dev.residue',
        OWNER,
        'with a residue larger than its TRB',
        process=KERNEL,
        source=f'{USB}/SwitchXUDCData.cpp {OFFLOAD}',
    ),
    Check(
        'usb.dev.broken',
        OWNER,
        ('SwitchUSB: XUDC event ring full', 'SwitchUSB: XUDC system error (ST.DSE)', ') not activated: '),
        process=KERNEL,
        regex=r'SwitchUSB: (?P<what>[^(:]+)',
        show=('what',),
        source=f'{USB}/SwitchXUDCEvents.cpp, {USB}/SwitchXUDCEndpoints.cpp',
    ),
    Check(
        'usb.dev.odd-events',
        OWNER,
        'SwitchUSB: XUDC transfer event ',
        REPORT,
        process=KERNEL,
        regex=r'transfer event (?P<what>with an error|for no request on the ring) on',
        show=('what',),
        source=f'{USB}/SwitchXUDCData.cpp {OFFLOAD}',
    ),
    Check(
        'usb.dev.stats',
        OWNER,
        'SwitchXUDC: stats ',
        REPORT,
        process=KERNEL,
        regex=STATS.pattern,
        source=f'{USB}/SwitchXUDCStats.cpp {OFFLOAD}',
        note=BASELINE,
    ),
]


def stats_row(result: Result) -> Result:
    """Return the stats lines summed: the busiest window's CPU and request rate, bounces by reason, serial requests."""
    check = Check(
        'usb.dev.stats-summary', OWNER, result.check.contains, REPORT, source=result.check.source, note=BASELINE
    )
    windows = [m for m in result.matches if 'cpu' in m]
    if not windows:
        return Result(check, NO_DATA, 'no stats lines (development builds, after data moved)')

    def rate(m: dict) -> float:
        return (int(m['in_req']) + int(m['out_req'])) * 1000 / max(int(m['ms']), 1)

    busiest = max(windows, key=rate)
    total = {k: sum(int(m[k]) for m in windows) for k in ('breaks', 'k64', 'high', 'edges', 'in_serial', 'out_serial')}
    observed = (
        f'{len(windows)} windows; busiest {rate(busiest):.0f} requests/s at CPU {busiest["cpu"]} %, '
        f'IN {busiest["in_mbs"]} OUT {busiest["out_mbs"]} MB/s; bounced packets {total["breaks"]} breaks, '
        f'{total["k64"]} 64K, {total["high"]} above 4G, {total["edges"]} cache-line edges; serial IN '
        f'{total["in_serial"]} OUT {total["out_serial"]} (baseline {BASELINE})'
    )
    return Result(check, INFO, observed, windows)


def no_partner_row(by_check: dict[str, Result]) -> Result:
    """Return the connects the XUDC reported with the port empty: after a PD detach and before the next attach."""
    check = Check(
        'usb.dev.no-partner',
        OWNER,
        'SwitchUSB: host connected (',
        'count == 0',
        source='usb.dev.connects, usb.pd.gone, usb.pd.came',
    )
    connects = by_check['usb.dev.connects'].matches if 'usb.dev.connects' in by_check else []
    if not connects:
        return Result(check, NO_DATA, 'no host connected line')
    opens, closes = (by_check[name].matches if name in by_check else [] for name in PORT_EMPTY)
    found = [(m, span_opener(m, opens, closes)) for m in connects]
    found = [(m, opener) for m, opener in found if opener]
    if not found:
        return Result(check, PASS, f'{len(connects)} connects, each with a partner on the port')
    parts = [
        f'{clock(m["time"])} {m["message"].removeprefix("SwitchUSB: ")}, '
        f'{(m["time"] - opener["time"]).total_seconds():.3f} s after the PD detach at {clock(opener["time"])} '
        f'({opener["message"].removeprefix("SwitchPD: ").split(" (alert")[0]}), no attach between'
        for m, opener in found
    ]
    observed = 'device-mode connect reported with no partner: ' + '; '.join(parts)
    return Result(check, FAIL, observed, [m for m, _ in found])
