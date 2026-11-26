"""The XUSB host controller under AppleUSBXHCI: out of power state 0, its save state, exceptions and wakes from SC7.

Development builds log each power state change ("controller power state <from> -> <to>"); usb_sequence.py judges the
controller against the host transport from those. Without them the run is read from the development role line's
USBCMD.RS and from enumeration, the failure from Apple's powerOn deadline, save-state and exception lines. An exception
within 10 s of a wake is the expected report of a saved state SC7 took (0x100 lost power, maybe 0x4 beside it).
"""

from tools.devicerun.rules import REPORT, TIMELINE, Check

KERNEL = 'kernel'
OWNER = 'usb-host'
USB = 'drivers/SwitchUSB'
XHCI = f'{USB}/SwitchXHCI.cpp'
EVENTS = f'{USB}/SwitchXHCIEvents.cpp'
ROLE = f'{USB}/SwitchUSB.cpp'
SUSPEND = 'drivers/SwitchSuspend/SwitchSuspend.cpp'
APPLE_USB = 'IOUSBHostFamily, both versions (24B119 kernelcache strings, emulator usb-emu runs, boot-1109-2145)'
ROLE_REPORT = ' role +1 s (change '
HOST_REPORT = r'SwitchUSB: host role \+1 s \(change \d+(?:, 1/2)?\): xHCI USBCMD '
WAKE_S = 10.0

USB_CONTROLLER = [
    Check(
        'usb.ctrl.deadline',
        OWNER,
        'AppleUSBXHCIPort::powerOn: deadline passed (PORTSC',
        process=KERNEL,
        regex=r'(?P<port>\w+)@\w+: AppleUSBXHCIPort::powerOn: deadline passed \(PORTSC (?P<portsc>0x[0-9a-f]+)\)',
        show=('port', 'portsc'),
        source=APPLE_USB,
        note='the controller is at power state 0: AppleUSBXHCI reads every register as 0',
    ),
    Check(
        'usb.ctrl.save-state',
        OWNER,
        'AppleUSBHostController::setPowerStateGated: going to state',
        process=KERNEL,
        regex=r'going to state (?P<state>\d+) returned (?P<error>0xe[0-9a-f]+)',
        show=('state', 'error'),
        source=APPLE_USB,
    ),
    Check(
        'usb.ctrl.exception',
        OWNER,
        'hardwareExceptionThreadCallGated: 0x',
        process=KERNEL,
        regex=r': (?P<code>0x[0-9a-f]+) \((?P<name>[^)]*)\)',
        show=('code', 'name'),
        unless='usb.sleep.wake',
        unless_within=WAKE_S,
        source=APPLE_USB,
        note='outside a wake; usb.sleep.recoveries lists the ones within 10 s of a wake',
    ),
    Check(
        'usb.ctrl.exceptions',
        OWNER,
        'hardwareExceptionThreadCallGated: 0x',
        REPORT,
        process=KERNEL,
        regex=r': (?P<code>0x[0-9a-f]+) \((?P<name>[^)]*)\)',
        show=('code', 'name'),
        source=APPLE_USB,
        note='every code; usb.sleep.recoveries groups those within 10 s of a wake',
    ),
    Check(
        'usb.ctrl.recovery',
        OWNER,
        'hardwareExceptionThreadCallGated: attempting recovery',
        REPORT,
        process=KERNEL,
        source=APPLE_USB,
    ),
    Check(
        'usb.sleep.wake',
        OWNER,
        'SwitchSuspend: wake ',
        TIMELINE,
        process=KERNEL,
        regex=r'SwitchSuspend: wake (?P<wake>\d+): ',
        source=SUSPEND,
    ),
    Check(
        'usb.ctrl.sc7-restore', OWNER, 'SwitchUSB: XUSB host restore after SC7', TIMELINE, process=KERNEL, source=XHCI
    ),
    Check(
        'usb.ctrl.power',
        OWNER,
        'SwitchUSB: controller power state ',
        TIMELINE,
        process=KERNEL,
        regex=r'state (?P<before>\d+) -> (?P<after>\d+)$',
        source=EVENTS,
        note='0 Off, 1 Sleep, 2 low power, 3 On; development builds',
    ),
    Check(
        'usb.role.host-report',
        OWNER,
        ROLE_REPORT,
        REPORT,
        process=KERNEL,
        regex=HOST_REPORT,
        source=ROLE,
        note='development builds only',
    ),
    Check(
        'usb.ctrl.running',
        OWNER,
        ROLE_REPORT,
        'count > 0',
        process=KERNEL,
        regex=HOST_REPORT + r'0x[0-9a-f]*[13579bdf] ',
        needs='usb.role.host-report',
        source=ROLE,
        note='USBCMD.RS in a host-role line',
    ),
    Check(
        'usb.ctrl.halted',
        OWNER,
        ROLE_REPORT,
        process=KERNEL,
        regex=HOST_REPORT + r'(?P<usbcmd>0x[0-9a-f]*[02468ace]|0) USBSTS (?P<usbsts>\S+),',
        show=('usbcmd', 'usbsts'),
        after='usb.host.role',
        after_min=0.9,
        within=2.0,
        source=ROLE,
        note=(
            'USBCMD.RS clear 1 s after a host role began; a report printed sooner belongs to an earlier change, its '
            'registers read before the role (boot 2026-11-14 19:17: 0.39 s after, the PD controller still empty)'
        ),
    ),
]
