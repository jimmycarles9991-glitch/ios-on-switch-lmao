"""The USB-C port's power role: the BM92T36 attach, its classify window, VBUS for a sink-only partner, the swap.

Every decision is one SwitchPD line (SwitchPDPolicy.h); the data role swap decision lines come from the alternate-mode
engine.
"""

from tools.devicerun.rules import REPORT, TIMELINE, Check

KERNEL = 'kernel'
OWNER = 'usb-host'
USB = 'drivers/SwitchUSB'
PD = f'{USB}/SwitchPD.cpp'
PORT_EMPTY = ('usb.pd.gone', 'usb.pd.came')
SETTINGS = f'{USB}/SwitchPDSettings.cpp'
POLICY = f'{USB}/SwitchPDPolicy.h'
ALT = 'drivers/SwitchDisplayPort/SwitchPDAltMode.cpp, drivers/SwitchDisplayPort/SwitchPDAltModeSteps.h'

USB_PD = [
    Check('usb.pd.started', OWNER, 'SwitchPD: started (', TIMELINE, process=KERNEL, source=PD),
    Check('usb.pd.unusable', OWNER, 'SwitchPD: BM92T36 not usable', process=KERNEL, source=PD),
    Check(
        'usb.pd.classified',
        OWNER,
        'the partner came up as our sink',
        process=KERNEL,
        regex=r'SwitchPD: (?:classifying|waiting for source): the partner came up as our sink',
        followed='usb.pd.decision',
        within=3.0,
        failure='a classify or source-wait window ended with no decision',
        source=POLICY,
    ),
    Check(
        'usb.pd.decision',
        OWNER,
        (
            'SwitchPD: sink-only partner: sourcing VBUS',
            'SwitchPD: dual-role partner: ',
            'SwitchPD: not sourcing: ',
            'SwitchPD: empty: the sink-only partner did not come back',
            'SwitchPD: sink: the partner came back as a source',
        ),
        REPORT,
        process=KERNEL,
        regex=r'SwitchPD: (?P<decision>[^:(]+)',
        show=('decision',),
        source=f'{POLICY}, {PD}',
    ),
    Check(
        'usb.pd.gone',
        OWNER,
        ('SwitchPD: detached', 'SwitchPD: empty: the sink-only partner did not come back'),
        TIMELINE,
        process=KERNEL,
        source=f'{POLICY}, {PD}',
        note='the port empty until usb.pd.came',
    ),
    Check(
        'usb.pd.came',
        OWNER,
        ('SwitchPD: sink: the partner ', 'the partner came up as our sink'),
        TIMELINE,
        process=KERNEL,
        regex=r'SwitchPD: (?:sink: the partner |(?:classifying|waiting for source): the partner came up as our sink)',
        source=f'{POLICY}, {PD}',
        note='a partner on the empty port',
    ),
    Check(
        'usb.pd.sourcing',
        OWNER,
        'SwitchPD: sink-only partner: sourcing VBUS',
        'count > 0',
        process=KERNEL,
        needs='usb.pd.classified',
        source=PD,
        note='plug the stick or keyboard (manual steps)',
    ),
    Check(
        'usb.pd.failed',
        OWNER,
        (
            'SwitchPD: not sourcing: ',
            'SwitchPD: empty: the sink-only partner did not come back',
            'SwitchPD: sink: no PD events',
            'SwitchPD: BM92T36 ALERT (PK4) fired',
            'SwitchPD: source switches ',
            'SwitchPD: source rails ',
            'SwitchPD: status read failed',
            'SwitchPD: source fault: PD hard reset',
            'SwitchPD: over-current protection on failed',
            'SwitchPD: CONFIG2 or ALERT_ENABLE not readable',
            'SwitchBattery: OTG boost ',
        ),
        process=KERNEL,
        regex=r'Switch(?:PD|Battery): (?P<why>[^(]+)',
        show=('why',),
        source=f'{POLICY}, {PD}, {SETTINGS}, drivers/SwitchBattery/SwitchBattery.cpp',
    ),
    Check(
        'usb.pd.ufp-write',
        OWNER,
        'SwitchPD: incoming ',
        process=KERNEL,
        regex=r'incoming .*write failed',
        source=SETTINGS,
    ),
    Check(
        'usb.pd.settings',
        OWNER,
        (
            'SwitchPD: BM92T36 settings ',
            'SwitchPD: leaving the BM92T36 as this boot set it',
            'SwitchPD: no Shut Down notification',
        ),
        process=KERNEL,
        regex=r'(?P<what>not all back|leaving the BM92T36|no Shut Down notification)',
        show=('what',),
        source=SETTINGS,
        note='Shut Down and Restart put the BM92T36 back as the boot found it',
    ),
    Check(
        'usb.pd.sys-reset',
        OWNER,
        'SwitchPD: BM92T36 SYS_RESET',
        process=KERNEL,
        regex=r'SYS_RESET(?P<failed> failed)?: (?P<why>(?!found plugged as DFP).*) \(status1',
        show=('why', 'failed'),
        source=PD,
        note='the start-up reset of a controller left as DFP is usb.pd.start-reset',
    ),
    Check(
        'usb.pd.start-reset',
        OWNER,
        'SwitchPD: BM92T36 SYS_RESET',
        TIMELINE,
        process=KERNEL,
        regex='found plugged as DFP',
        source=PD,
    ),
    Check(
        'usb.pd.alt',
        OWNER,
        'SwitchPD: alternate mode (',
        TIMELINE,
        process=KERNEL,
        source=ALT,
        note='the data role swap decision',
    ),
    Check(
        'usb.pd.request-aborted',
        OWNER,
        'SwitchPD: request aborted at attach',
        REPORT,
        process=KERNEL,
        source=f'{USB}/SwitchPDRequest.cpp',
        note='a Mac on the port: the PD request the controller aborted while the partner attached, redone',
    ),
    Check(
        'usb.pd.alt-failed',
        OWNER,
        'SwitchPD: alternate mode (',
        process=KERNEL,
        regex=r'\(failed\): (?P<why>.*)$',
        show=('why',),
        source=ALT,
    ),
]
