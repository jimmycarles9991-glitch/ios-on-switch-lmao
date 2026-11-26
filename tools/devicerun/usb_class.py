"""USB host mode past configuration: what proves a class driver took its device, SuperSpeed, and device-only values.

Storage shows as an identified disk; HID through the DriverKit dext on iOS 27 (the kernel driver only after the dext's
120 s launch timeout) or the kernel driver on iOS 17; a hub as a device behind it enumerating; the dext-only classes
(serial adapters, wired Xbox pads) as their dext checking in.
"""

from tools.devicerun.rules import REPORT, TIMELINE, Check
from tools.devicerun.usb_controller import ROLE_REPORT
from tools.devicerun.usb_host import (
    APPLE_USB,
    ENUMERATED,
    EVENTS,
    HOST,
    KERNEL,
    MAILBOX_COMMAND,
    OWNER,
    RESET_SSPI,
    ROLE,
    USB,
)
from tools.devicerun.usb_pd import SETTINGS

DK = 'kernel/xnu-12377.121.6/iokit/Kernel/IOUserServer.cpp'
DK_MATCHING = 'kernel/xnu-12377.121.6/iokit/Kernel/IOService.cpp'
SHARED_REGION = 'kernel/xnu-12377.121.6/bsd/vm/vm_unix.c'
HID_DEXT_S = 120.0
HID_FALLBACK_S = 3 * HID_DEXT_S + 40.0
DK_SERVICE = r'.+?-0x[0-9a-f]+'
SS_RESET = (
    r'SuperSpeed reset (?:(?P<what>requested|taken) on port (?P<port>\d+)'
    r'|on port (?P<dport>\d+) (?P<deferred>deferred)):? .*?\(PORTSC (?P<before>0x[0-9a-f]+|0)'
    r'(?: -> (?P<after>0x[0-9a-f]+|0))?(?:, RESET_SSPI (?P<sspi>taken|refused))?\)$'
)

CLASS_CHECKS = [
    Check(
        'usb.class.storage',
        OWNER,
        'identified disk',
        REPORT,
        process=KERNEL,
        debug=True,
        regex=r'identified (?P<disk>disk\d+)',
        show=('disk',),
        source='IOStorageFamily (emulator usb-emu hub runs)',
        note='after the stick step, NO-DATA here is the storage failure',
    ),
    Check(
        'usb.class.hid-dext',
        OWNER,
        'waiting for server',
        REPORT,
        process=KERNEL,
        regex=rf'^DK: (?P<service>{DK_SERVICE}) waiting for server \S*AppleUserHIDDrivers\S*-[0-9a-f]+$',
        source=DK,
        note='iOS 27: the HID dext asked for; usb.class.driver judges it',
    ),
    Check(
        'usb.class.hid-dext-failed',
        OWNER,
        'failed to find server',
        REPORT,
        process=KERNEL,
        regex=rf'^DK: (?P<service>{DK_SERVICE}) failed to find server (?P<server>\S*AppleUserHIDDrivers\S*)-'
        r'(?P<tag>[0-9a-f]+), remaining \d+ms, retries (?P<retries>\d+)',
        source=DK,
        note='one launch of the HID dext that did not check in; the services waiting on one server each print it',
    ),
    Check(
        'usb.dext.launch-failed',
        OWNER,
        'failed to launch server',
        REPORT,
        process=KERNEL,
        regex=rf'^DK: (?P<service>{DK_SERVICE}) failed to launch server$',
        show=('service',),
        source=DK_MATCHING,
        note='the kernel gave up on the dext for this service; its kernel driver matches next',
    ),
    Check(
        'usb.dext.shared-region',
        OWNER,
        "not on process' root volume nor preboot volume",
        process=KERNEL,
        regex=r"^shared_region: \S+ \[(?P<pid>\d+)\((?P<process>[^)]*)\)\] map\(\S+:'(?P<cache>[^']*)'\): not on",
        show=('process', 'cache'),
        failure="the kernel refused a process its dyld shared cache (not on process' root volume nor preboot volume)",
        source=SHARED_REGION,
        note='each is a dext launch that died before checking in (DriverKit processes are named com.apple.AppleU...)',
    ),
    Check(
        'usb.class.hid-deferred',
        OWNER,
        'matching deferred by ',
        REPORT,
        process=KERNEL,
        regex='by IOUSBHostHIDDevice',
        source='kernel/xnu-12377.121.6/iokit/Kernel/IOService.cpp',
        note='a HID interface that matched before the IOKit daemon had loaded the dexts (a keyboard on at boot)',
    ),
    Check(
        'usb.class.hid-kernel',
        OWNER,
        'IOUSBHostHIDDevice:',
        REPORT,
        process=KERNEL,
        debug=True,
        regex=r'^IOUSBHostHIDDevice:(?P<hid>0x[0-9a-f]+) start$',
        source='IOHIDFamily (emulator usb-emu iOS 17 runs, device boot 2026-11-14 19:17)',
        note=(
            'iOS 17, and iOS 27 after the dext failed; usb.class.driver needs one opened per device (a FIDO interface '
            'has no event driver to open it)'
        ),
    ),
    Check(
        'usb.class.hid-open',
        OWNER,
        ' open by ',
        REPORT,
        process=KERNEL,
        debug=True,
        regex=r'^(?!Switch|IOHIDUserDevice)(?P<device>\w+):(?P<hid>0x[0-9a-f]+) open by (?P<client>\w+)',
        show=('device', 'client'),
        source='IOHIDFamily (IOUSBHostHIDDevice in the emulator, SwitchHIDDevice on the device)',
        note="a HID device published and opened; the Switch's own and user-space HID devices are left out",
    ),
    Check(
        'usb.class.hub',
        OWNER,
        'IOUSBHostDevice::setConfigurationGated: ',
        process=KERNEL,
        debug=True,
        regex=r'IOUSBHostDevice::setConfigurationGated: AppleUSB[23]0Hub selected',
        followed='usb.enum.hub-port',
        within=10.0,
        failure='a hub configured and no device behind it enumerated within 10 s',
        source=APPLE_USB,
    ),
    Check(
        'usb.class.hub-port-disabled',
        OWNER,
        'AppleUSBHostPort::terminateDevice: destroying',
        process=KERNEL,
        debug=True,
        regex=r'^AppleUSB[23]0HubPort@\w+: \S+ destroying \S+ \((?P<product>[^)]*)\): hardware port disabled',
        show=('product',),
        source=APPLE_USB,
        note="a device behind a hub lost its port; the emulator's hub model does this once per device (being fixed)",
    ),
    Check(
        'usb.enum.hub-port',
        OWNER,
        ENUMERATED,
        REPORT,
        process=KERNEL,
        debug=True,
        regex=r'^AppleUSB[23]0HubPort@',
        source=APPLE_USB,
    ),
    Check(
        'usb.dext.wait',
        OWNER,
        'waiting for server',
        REPORT,
        process=KERNEL,
        regex=rf'^DK: (?P<service>{DK_SERVICE}) waiting for server (?P<server>.+)-(?P<tag>[0-9a-f]+)$',
        show=('server',),
        source=DK,
    ),
    Check(
        'usb.dext.launched',
        OWNER,
        'server launched, validating',
        REPORT,
        process=KERNEL,
        regex=rf'^DK: (?P<service>{DK_SERVICE}) server launched',
        show=('service',),
        source=DK,
    ),
    Check(
        'usb.dext.start',
        OWNER,
        '::start(',
        REPORT,
        process=KERNEL,
        regex=rf'^DK: (?P<service>{DK_SERVICE})::start\((?P<provider>{DK_SERVICE})\) (?P<result>ok|fail)$',
        show=('service', 'result'),
        source=DK,
        note="every dext service's start, a server reused or not",
    ),
    Check(
        'usb.class.driverkitd',
        OWNER,
        'Received MIG message',
        REPORT,
        process='driverkitd',
        debug=True,
        source='driverkitd',
        note='a count only: these come at host attach too, before any device, so they do not show a dext request',
    ),
    Check(
        'usb.dext.waits-answered',
        OWNER,
        'waiting for server',
        process=KERNEL,
        regex=rf'^DK: (?P<service>{DK_SERVICE}) waiting for server (?!\S*AppleUserHIDDrivers)',
        followed='usb.dext.launched',
        within=HID_DEXT_S,
        pending_at_end=True,
        key='service',
        failure='a dext was asked for and did not check in within 120 s',
        source=DK,
        note='a dext other than the HID one: serial adapters and wired Xbox pads are dexts only',
    ),
    Check(
        'usb.dext.failed',
        OWNER,
        'failed to find server',
        process=KERNEL,
        regex=r'failed to find server (?P<server>(?!\S*AppleUserHIDDrivers).+)-[0-9a-f]+, remaining',
        show=('server',),
        source=DK,
    ),
]

SUPERSPEED_CHECKS = [
    Check('usb.ss.lane', OWNER, 'SwitchUSB: SuperSpeed lane ', TIMELINE, process=KERNEL, source=ROLE),
    Check(
        'usb.ss.lane-up',
        OWNER,
        'SwitchUSB: SuperSpeed lane up',
        'count > 0',
        process=KERNEL,
        needs='usb.ss.lane',
        source=ROLE,
    ),
    Check(
        'usb.ss.enumerated',
        OWNER,
        ENUMERATED,
        REPORT,
        process=KERNEL,
        regex=r'enumerated (?P<ids>\S+) \((?P<product>.*) / \d+\) at '
        r'(?P<speed>5000\s*Mbps|5\s*Gb(?:ps|/s)|SuperSpeed)$',
        show=('ids', 'product', 'speed'),
        source=APPLE_USB,
        note='actual SuperSpeed enumeration; lane power and RESET_SSPI alone do not establish a USB 3 link',
    ),
    Check(
        'usb.ss.u0',
        OWNER,
        'U0 SuperSpeed 5 Gb/s',
        REPORT,
        process=KERNEL,
        source='the role +1 s line (XusbPorts.h link and speed names)',
        note='a USB 3 device on the port',
    ),
    Check(
        'usb.ss.uphy-uncalibrated',
        OWNER,
        'not calibrated by PCIe yet',
        process=KERNEL,
        needs='usb.ss.lane',
        source=f'{USB}/XusbSuperSpeed.cpp',
    ),
    Check(
        'usb.ss.reset-sspi',
        OWNER,
        ('SwitchUSB: XUSB falcon did not take RESET_SSPI', 'SwitchUSB: SuperSpeed root port '),
        process=KERNEL,
        regex=r'RESET_SSPI|still powered',
        source=EVENTS,
    ),
    Check(
        'usb.ss.mailbox',
        OWNER,
        ('SwitchUSB: falcon mailbox ', 'SwitchUSB: falcon NAK '),
        process=KERNEL,
        regex=rf'SwitchUSB: (?P<what>{MAILBOX_COMMAND}{RESET_SSPI}\b[^(]*)',
        show=('what',),
        failure="RESET_SSPI's mailbox command failed",
        source=f'{EVENTS}, {HOST}',
        note='24B119 device boot 2026-11-14 (an earlier build): not taken in 250 ms, the ACK held off by the sender',
    ),
    Check(
        'usb.ss.resets',
        OWNER,
        'SwitchUSB: SuperSpeed reset ',
        TIMELINE,
        process=KERNEL,
        regex=SS_RESET,
        source=EVENTS,
        note='requested, deferred and taken; development builds',
    ),
    Check(
        'usb.ss.refused',
        OWNER,
        'SwitchUSB: SuperSpeed reset taken on port ',
        process=KERNEL,
        regex=r'RESET_SSPI refused\)$',
        failure='the falcon refused RESET_SSPI',
        source=EVENTS,
    ),
    Check('usb.ss.off', OWNER, 'SwitchUSB: SuperSpeed off: ', process=KERNEL, source=ROLE),
]

VALUE_CHECKS = [
    Check(
        'usb.info.capabilities',
        OWNER,
        'SwitchUSB: XUSB firmware ',
        REPORT,
        process=KERNEL,
        regex=r'HCIVERSION (?P<hciversion>\S+) HCSPARAMS1 (?P<hcs1>\S+) HCSPARAMS2 (?P<hcs2>\S+) HCCPARAMS1 '
        r'(?P<hcc1>\S+) \(AC64 (?P<ac64>\d) PPC (?P<ppc>\d) MaxPSASize (?P<psa>\d+)\) DBOFF (?P<dboff>\S+) '
        r'RTSOFF (?P<rtsoff>\S+), USB 2\.0 HLC (?P<hlc>\d)',
        show=('hciversion', 'hcs1', 'hcs2', 'hcc1', 'ac64', 'ppc', 'psa', 'dboff', 'rtsoff', 'hlc'),
        source=HOST,
        note='development builds; emulator: 0x100 0x9000124 0xf 0x87009 1 1 7 0x2000 0x1000 0',
    ),
    Check(
        'usb.info.settle',
        OWNER,
        ' not done after ',
        REPORT,
        process=KERNEL,
        regex=r'SwitchUSB: (?P<step>.+) not done after (?P<first>\d+) ms, (?P<outcome>done|still not done) after '
        r'(?P<bound>\d+) ms',
        show=('step', 'first', 'outcome', 'bound'),
        source=f'{USB}/XusbSettle.h',
        note='a first look that came too early: the falcon load, CNR, the SuperSpeed power-off (development builds)',
    ),
    Check(
        'usb.info.pd-as-found',
        OWNER,
        'SwitchPD: incoming ',
        REPORT,
        process=KERNEL,
        regex=r'\(CONFIG2 (?P<config2>[^ ,]+)[^,]*,(?: DP_ALERT_EN [^,]*,)? ALERT_ENABLE (?P<alert>[^ )]+)',
        show=('config2', 'alert'),
        source=SETTINGS,
        note='the values as the boot found them',
    ),
    Check(
        'usb.info.pd-restored',
        OWNER,
        'SwitchPD: BM92T36 settings ',
        REPORT,
        process=KERNEL,
        regex=r'settings (?P<outcome>back|not all back) as found \(CONFIG1 (?P<config1>\S+) CONFIG2 (?P<config2>\S+) '
        r'VENDOR_CONFIG (?P<vendor>\S+) ALERT_ENABLE (?P<alert>[^)]+)\)',
        show=('outcome', 'config2', 'alert'),
        source=SETTINGS,
        note='Shut Down and Restart',
    ),
    Check(
        'usb.info.port-current',
        OWNER,
        'kUSBWakePortCurrentLimit ',
        REPORT,
        process=KERNEL,
        debug=True,
        regex=r'^(?P<port>\w+)@\w+: \S+ kUSBWakePortCurrentLimit (?P<ma>\d+) mA',
        show=('port', 'ma'),
        source=APPLE_USB,
        note='at most 900 mA on the SuperSpeed port and 500 mA on the USB 2.0 port; a debug line, may be absent',
    ),
    Check(
        'usb.role.report',
        OWNER,
        ROLE_REPORT,
        REPORT,
        process=KERNEL,
        regex=r'SwitchUSB: (?P<role>host|device) role \+1 s \(change \d+(?:, 1/2)?\): xHCI USBCMD (?P<usbcmd>\S+) '
        r'USBSTS (?P<usbsts>\S+),',
        show=('role', 'usbcmd', 'usbsts'),
        source=ROLE,
        note='development builds',
    ),
]

USB_CLASS = CLASS_CHECKS + SUPERSPEED_CHECKS + VALUE_CHECKS
