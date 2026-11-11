"""Joy-Cons on the rails and over Bluetooth: the pairing that opens every rail session, and the page after a detach.

Boot 18 (11-09, iOS 27) is the baseline: both Joy-Cons, paired 2-3 s into the session the console's boot had
handshaken, were pulled off minutes later (16:51:56 R, 16:52:40 L) and never paged the console; paired as their next
attach opened, both paged within 0.5 s of leaving the rail. Pages seen on the console come 0.2-1.4 s after the detach
(boots 16 and 18).
"""

from tools.devicerun.rules import REPORT, Check

KERNEL = 'kernel'
RAIL = 'drivers/SwitchJoyCon/SwitchJoyConRail.cpp'
LINKS = 'drivers/SwitchBluetooth/SwitchBluetoothLinks.h'
DETACHED = 'on the rail stopped answering'
GAMEPAD_REQUEST = r'Connection Request (?P<address>[0-9a-f:]{17}) class 000508'
PAGE_WINDOW_S = 3.0

JOYCON = [
    # Every attach reads the Joy-Con's pairing log and says what its slot in use holds.
    Check(
        'joycon.slot-read',
        'r108-btpair',
        'SwitchJoyCon: Joy-Con',
        'count >= 1',
        process=KERNEL,
        regex=r'(?:slot (?P<slot>\d+) in use names (?P<host>[0-9a-f:]+), '
        r'host capability (?P<caps>0x[0-9a-f]+)|no slot in use)',
        show=('slot', 'host', 'caps'),
        source=f'{RAIL} finishPairing',
    ),
    # Every attach pairs the Joy-Con over the rail: one whose slot already named the console, left as it was, never
    # paged after a detach (boot 17).
    Check(
        'joycon.pairing-skipped',
        'r108-btexchange',
        'SwitchJoyCon: Joy-Con',
        process=KERNEL,
        regex=r'paired to [0-9a-f:]+ (?!over the rail)',
        needs='joycon.slot-read',
        source=f'{RAIL} finishPairing',
    ),
    Check(
        'joycon.detached',
        'r108-btpair',
        DETACHED,
        REPORT,
        process=KERNEL,
        regex=r'Joy-Con \((?P<side>[LR])\) (?:(?P<address>[0-9a-f:]{17}) \((?P<paired>[a-z ]+)\) )?on the rail',
        show=('side', 'paired'),
        source=f'{RAIL} tick',
    ),
    # A session the console's boot had handshaken ends when the address comes, so its pairing opens the next.
    Check(
        'joycon.session-not-ended',
        'r134-joycon',
        'did not answer the end of its session',
        process=KERNEL,
        source=f'{RAIL} timerAction',
    ),
    # A Joy-Con off the rail pages the console (a host page to it times out unless it is in sync mode).
    Check(
        'joycon.bt-paged',
        'r108-btpair',
        'link <- Connection Request',
        'count >= 1',
        process=KERNEL,
        regex=GAMEPAD_REQUEST,
        show=('address',),
        needs='joycon.detached',
        source=LINKS,
        note='boot 16: R and L paged at 04:05 after a button press',
    ),
    # Every Joy-Con paired as its rail session opened pages the console as soon as it leaves the rail, dated by its
    # last reply (the line comes 1.5 s of silence later).
    Check(
        'joycon.paged-on-detach',
        'r134-joycon',
        DETACHED,
        process=KERNEL,
        regex=r'Joy-Con \([LR]\) (?P<address>[0-9a-f:]{17}) \(paired as its session opened\) on the rail stopped '
        r'answering after \d+ input reports: \d+ requests unanswered over (?P<ms>\d+) ms',
        followed='joycon.bt-paged',
        key='address',
        since='ms',
        within=PAGE_WINDOW_S,
        show=('address',),
        source=f'{RAIL} tick, {LINKS}',
        note='each detach after a pairing: a Connection Request from that Joy-Con within 3 s',
    ),
    # The plugin's device info comes back in an input report (0x21): reports reach GameController over Bluetooth.
    Check(
        'joycon.bt-device-info',
        'r134-joycon',
        "Device is a 'Joy-Con",
        REPORT,
        process='backboardd',
        subsystem='com.apple.GameController',
        source='JoyConHIDServicePlugin (boot 18)',
    ),
    Check(
        'joycon.bt-input',
        'r134-joycon',
        'link <- Connection Request',
        process=KERNEL,
        regex=GAMEPAD_REQUEST,
        followed='joycon.bt-device-info',
        within=PAGE_WINDOW_S,
        show=('address',),
        source=LINKS,
        note="each Joy-Con page: JoyConHIDServicePlugin's device info over Bluetooth within 3 s (boot 18: 0.6-1.0 s)",
    ),
    Check(
        'joycon.bt-link-rx',
        'r134-joycon',
        'link <- Disconnection Complete',
        REPORT,
        process=KERNEL,
        regex=r'reason (?P<reason>0x[0-9a-f]+): rx (?P<rx>\d+)',
        show=('reason', 'rx'),
        source=LINKS,
        note='ACL packets each link received; a Joy-Con put back on the rail ends its link with 0x13',
    ),
    # bluetoothd without a key for a device that connected: a first pairing in Settings, or a Joy-Con whose rail key
    # joyconpaird did not hand over.
    Check(
        'joycon.bt-key-missing',
        'r108-btpair',
        'link -> Link Key Request Negative Reply',
        REPORT,
        process=KERNEL,
        regex=r'Negative Reply (?P<address>[0-9a-f:]+)',
        show=('address',),
        source=LINKS,
    ),
]
