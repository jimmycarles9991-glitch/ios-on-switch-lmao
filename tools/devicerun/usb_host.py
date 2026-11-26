"""USB host mode on the USB-C port from bring-up to configuration: the checks usb.py orders into layers."""

from tools.devicerun.rules import REPORT, TIMELINE, Check
from tools.devicerun.usb_controller import USB_CONTROLLER
from tools.devicerun.usb_pd import USB_PD
from tools.devicerun.usb_port import ROOT_PORT, USB_PORT

KERNEL = 'kernel'
OWNER = 'usb-host'
USB = 'drivers/SwitchUSB'
ROLE = f'{USB}/SwitchUSB.cpp'
XHCI = f'{USB}/SwitchXHCI.cpp'
EVENTS = f'{USB}/SwitchXHCIEvents.cpp'
HOST = f'{USB}/XusbHost.cpp'
APPLE_USB = 'IOUSBHostFamily, both versions (24B119 kernelcache strings, emulator usb-emu runs, boot-1109-2145)'
FOLD = (
    'Address Device or the first descriptor read timed out: the firmware does not deliver events targeted at '
    'interrupter 1 (the interrupter fold: 3 shown to AppleUSBXHCI, 1 in hardware)'
)

ENUMERATED = 'AppleUSBHostPort::enumerateDeviceComplete'
ENUMERATION = r'(?P<port>\w+)@(?P<loc>[0-9a-f]+): AppleUSBHostPort::enumerateDeviceComplete\w*: enumerated '
ENUMERATION += r'(?P<ids>\S+) \((?P<product>.*) / \d+\) at (?P<speed>.+)$'
DEVICE_ON_PORT = 'SwitchUSB: host role (a device on the port, VBUS on)'
# A control IN transfer that completed without error and handed back bytes the device never sent: a descriptor of
# another type (the buffer still holding an earlier read) or zeros (a device descriptor with bMaxPacketSize0 0).
STALE = (
    r'^(?:.*@(?P<loc>[0-9a-f]+): )?.*?(?:type 0x(?P<want>[0-9a-f]{2}) index \d+ length \d+: completed with '
    r'0x00000000 bytesTransferred \d+ type 0x(?!(?P=want))(?P<got>[0-9a-f]{2})$'
    r'|(?P<fragment>device descriptor fragment is invalid)$|endpoint 0x00 invalid wMaxPacketSize (?P<mps>0x0000)$)'
)
STALE_LINES = (
    ': completed with 0x00000000 bytesTransferred ',
    ': device descriptor fragment is invalid',
    ': endpoint 0x00 invalid wMaxPacketSize 0x0000',
)

# A mailbox command that failed, by its number (SwitchXHCI::sendCommand): not taken by the deadline, the mailbox busy,
# or a NAK. The rest of the mailbox's failures belong to bring-up whatever command was out.
RESET_SSPI = 16
MAILBOX_COMMAND = r'falcon (?:mailbox |mailbox busy for |NAK \S+ to mailbox )command '
MAILBOX_OTHER = (
    r'XUSB falcon did not take MSG_ENABLED|falcon mailbox reply |unknown XUSB falcon mailbox message'
    r'|XUSB mailbox interrupt \(INTID|XUSB falcon firmware hang|XUSB host did not come back from SC7'
    r'|cannot attach the XUSB mailbox interrupt|falcon (?:ACK|NAK) \S+ with no command waiting'
)
MAILBOX_LINES = (
    'SwitchUSB: XUSB falcon did not take MSG_ENABLED',
    'SwitchUSB: falcon mailbox ',
    'SwitchUSB: falcon NAK ',
    'SwitchUSB: falcon ACK ',
    'SwitchUSB: unknown XUSB falcon mailbox message',
    'SwitchUSB: XUSB mailbox interrupt (INTID',
    'SwitchUSB: XUSB falcon firmware hang',
    'SwitchUSB: XUSB host did not come back from SC7',
    'SwitchUSB: cannot attach the XUSB mailbox interrupt',
)

BRINGUP_CHECKS = [
    Check(
        'usb.kernel',
        OWNER,
        'Darwin Kernel Version ',
        REPORT,
        process=KERNEL,
        regex=r'Version (?P<darwin>\d+)\.',
        show=('darwin',),
        source='xnu (25: iOS 27, 23: iOS 17)',
    ),
    Check(
        'usb.host.mode-off',
        OWNER,
        'SwitchUSB: host mode off: ',
        process=KERNEL,
        regex=r'off: (?P<why>.*)$',
        show=('why',),
        source=ROLE,
    ),
    Check(
        'usb.host.up',
        OWNER,
        'SwitchUSB: host controller up (',
        TIMELINE,
        process=KERNEL,
        source=XHCI,
        note='once per boot, at the first host role',
    ),
    Check(
        'usb.host.role',
        OWNER,
        'SwitchUSB: host role (',
        REPORT,
        process=KERNEL,
        regex=r'role \((?P<why>.*)\)$',
        show=('why',),
        source=ROLE,
    ),
    Check(
        'usb.host.answer',
        OWNER,
        ('SwitchUSB: host role (', 'SwitchUSB: host mode off: '),
        REPORT,
        process=KERNEL,
        source=ROLE,
    ),
    Check(
        'usb.host.silent',
        OWNER,
        'SwitchPD: sink-only partner: sourcing VBUS',
        process=KERNEL,
        followed='usb.host.answer',
        within=10.0,
        failure='VBUS went on for a sink-only partner and neither a host role nor host mode off followed within 10 s',
        source=f'{USB}/SwitchPD.cpp, {ROLE}',
    ),
    Check(
        'usb.host.roles',
        OWNER,
        ('SwitchUSB: host role (', 'SwitchUSB: device role'),
        TIMELINE,
        process=KERNEL,
        regex=r'role(?: \(|$)',
        source=ROLE,
    ),
    Check(
        'usb.host.not-loaded',
        OWNER,
        'did not load',
        process=KERNEL,
        regex=r'SwitchUSB: (?:host mode off: )?(?P<kext>\S+) did not load',
        show=('kext',),
        source=XHCI,
    ),
    Check(
        'usb.host.mailbox',
        OWNER,
        MAILBOX_LINES,
        process=KERNEL,
        regex=rf'SwitchUSB: (?P<what>(?:{MAILBOX_COMMAND}(?!{RESET_SSPI}\b)\d+|{MAILBOX_OTHER})[^(:,]*)',
        show=('what',),
        source=f'{HOST}, {EVENTS}, {XHCI}',
        note=(
            "the mailbox's own failures and MSG_ENABLED's; RESET_SSPI's are usb.ss.mailbox's. The panics reach the log "
            'only when a later boot keeps them; crash.panics and usb.panic read the .ips'
        ),
    ),
    Check(
        'usb.host.dma',
        OWNER,
        ('SwitchUSB: DMA ', 'SwitchUSB: host DMA by physical address with bounces (SMMU off: '),
        process=KERNEL,
        regex=r'SwitchUSB: (?P<what>DMA (?:bounce pool|mapping table) full|host DMA by physical address.*)',
        show=('what',),
        source=f'{USB}/UsbDmaMapper.cpp',
    ),
    Check('usb.host.firmware', OWNER, 'SwitchUSB: XUSB firmware ', TIMELINE, process=KERNEL, source=HOST),
]

ENUMERATION_CHECKS = [
    Check(
        'usb.enum.root',
        OWNER,
        ENUMERATED,
        'count > 0',
        process=KERNEL,
        debug=True,
        regex=rf'^(?={ROOT_PORT}@)' + ENUMERATION,
        show=('product', 'speed'),
        needs='usb.host.role',
        source=APPLE_USB,
    ),
    Check(
        'usb.enum.devices',
        OWNER,
        ENUMERATED,
        REPORT,
        process=KERNEL,
        debug=True,
        regex=ENUMERATION,
        show=('product', 'speed', 'port'),
        source=APPLE_USB,
    ),
    Check(
        'usb.enum.after-role',
        OWNER,
        DEVICE_ON_PORT,
        process=KERNEL,
        followed='usb.enum.root',
        within=10.0,
        failure='a device on the port and VBUS on, but nothing enumerated on a root port within 10 s',
        source=ROLE,
    ),
    Check(
        'usb.enum.fold',
        OWNER,
        'failed to create device (0x',
        process=KERNEL,
        debug=True,
        regex=r'failed to create device \((?P<code>0xe00002(?:d6|eb))\)',
        show=('code',),
        failure=FOLD,
        source=APPLE_USB,
        note='0xe00002d6 timeout, 0xe00002eb aborted',
    ),
    Check(
        'usb.enum.stale-lines',
        OWNER,
        STALE_LINES,
        REPORT,
        process=KERNEL,
        debug=True,
        regex=STALE,
        source='IOUSBHostFamily (24B119 device boot 2026-11-14 14:15)',
        note='usb.enum.stale-data judges them by enumeration attempt',
    ),
    Check(
        'usb.enum.failed',
        OWNER,
        (
            'failed to create device',
            ': enumeration failed',
            'persistent enumeration failures',
            'port not enabled (port status',
            'enumerated stale device',
        ),
        process=KERNEL,
        debug=True,
        regex=r'^(?:(?P<port>\w+)@(?P<loc>[0-9a-f]+): )?.*?(?P<what>failed to create device \(\S+\)|enumeration failed'
        r'|persistent enumeration failures|port not enabled \(port status \S+|enumerated stale device)',
        show=('what',),
        source=APPLE_USB,
    ),
    Check(
        'usb.enum.attempts',
        OWNER,
        ': enumeration attempt ',
        REPORT,
        process=KERNEL,
        debug=True,
        regex=r'enumeration attempt (?P<attempt>\d+)',
        show=('attempt',),
        source=APPLE_USB,
    ),
    Check(
        'usb.iokit.bad-busy-count',
        OWNER,
        'bad busy count',
        process=KERNEL,
        debug=True,
        regex=r'(?P<service>[^:]+): bad busy count',
        show=('service',),
        source='kernel/xnu-12377.121.6/iokit/Kernel/IOService.cpp',
        note='emulator iOS 17: a hub re-enumerating its stick',
    ),
    Check(
        'usb.enum.transaction-error',
        OWNER,
        ' (transaction error)',
        REPORT,
        process=KERNEL,
        regex=r'device \d+ \((?P<device>[^)]*)\) endpoint (?P<endpoint>0x[0-9a-f]+): status \S+ \(transaction error\)',
        show=('endpoint',),
        source='IOUSBHostFamily AppleUSBIORequest::complete (device boot 2026-11-14 19:17)',
        note='usb.watch.transaction-errors holds them against the teardowns',
    ),
    Check(
        'usb.enum.destroyed',
        OWNER,
        'AppleUSBHostPort::terminateDevice: destroying',
        REPORT,
        process=KERNEL,
        debug=True,
        regex=r'^(?P<port>\w+)@(?P<loc>\w+): \S+ destroying \S+ \((?P<product>[^)]*)\): (?P<why>.*)$',
        show=('product', 'why'),
        source=APPLE_USB,
    ),
    Check(
        'usb.enum.configured',
        OWNER,
        'IOUSBHostDevice::setConfigurationGated: ',
        'count > 0',
        process=KERNEL,
        debug=True,
        regex=r'^(?P<product>.*)@(?P<loc>[0-9a-f]+): IOUSBHostDevice::setConfigurationGated: (?P<driver>\S+) selected',
        show=('product', 'driver'),
        needs='usb.enum.root',
        source=APPLE_USB,
    ),
    Check(
        'usb.enum.configured-each',
        OWNER,
        ENUMERATED,
        process=KERNEL,
        debug=True,
        regex=ENUMERATION,
        followed='usb.enum.configured',
        within=5.0,
        key='loc',
        failure='a device enumerated and was given no configuration within 5 s',
        source=APPLE_USB,
    ),
]

USB_HOST = USB_PD + BRINGUP_CHECKS + USB_PORT + USB_CONTROLLER + ENUMERATION_CHECKS
