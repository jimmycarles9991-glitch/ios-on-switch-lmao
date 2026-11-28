"""iPadOS 27's motion capabilities: switch_ui_scale_sites_24B119.h's CoreMotion site.

CoreMotion takes J181's word without the motion coprocessor bits whose services nothing here provides. It keeps bit 4:
6-axis device motion comes from SwitchMotion's own fusion on {0xff0c, 5}, as from J181's coprocessor, so usage 5 is
asked for and must be ready; the other page-0xff0c services are never opened.

Boot 2 (22-23 Nov, iOS 27) is the baseline: 90 "not ready" lines for usages 1, 5 and 26 and 24 give-ups (locationd 2,
SpringBoard 15, backboardd 6, SwitchHWTest 1); locationd also added usages 4, 7, 8 and 24 without setting a property.
Usages 4, 8 and 24 are reported, not judged: their creation gates were not read. locationd has its own copy of the
table, outside the cache, which edits_24B119.json edits (15032ab57's trust cache trusts the result): it is judged on its
own, on iPadOS 27 boots only (motion.kernel), so iOS 17's verdicts stay as they were.

The fast path (SwitchHIDFastPath.cpp): 24B119's sandbox profiles let SpringBoard (by its user-client-class entitlement)
and locationd open IOHIDEventServiceFastPathUserClient, so their opens must succeed and their copies find a report
once a session has started. backboardd's profile admits the class only by that entitlement, which backboardd lacks, so
J181 refuses its open as well and its CoreMotion takes the pushed sample: reported, not judged.

On iPadOS 27 the accelerometer, gyroscope and device motion services carry J181's TCCService kTCCServiceMotionSensors
(drivers/SwitchTouch/SwitchHIDMotionAccess.cpp). IOKit's event system then matches them only for platform binaries or
connections TCC allows, and logs each connection it refuses. A refusal there means a motion client lost the services.
The log redacts the line's arguments as <private>; only these services carry a TCCService here, so a redacted one is
judged as theirs. A refusal of another shape would go unseen there, so a client that asks for a gated service and finds
none fails as well: CoreMotion then logs the usage pair "not ready", as boot 2 did for the services nothing provided
(motion.gated-not-ready for the accelerometer and gyroscope, motion.spu-not-ready and motion.locationd-spu-not-ready
for device motion). Clients add these services on demand, not every boot (24 Nov: backboardd added the accelerometer in
15 of 22 iPadOS 27 boots, SpringBoard in 19), so their adds alone prove nothing.
"""

from tools.devicerun.rules import REPORT, Check

OWNER = 'perf1-daemons'
SOURCE = 'kernel/xnu-12377.121.6/bsd/vm/switch_ui_scale_sites_24B119.h'
SUBSYSTEM = 'com.apple.locationd.Motion'
ADDED = '[CLIoHidInterface] Adding new Device with usage pair {65292, '
NOT_READY = (
    '[CLIoHidInterface] Service for device with usage pair {65292, ',
    '[CLIoHidInterface] Service for usage pair {65292, ',
)
GATED_NOT_READY = (
    '[CLIoHidInterface] Service for device with usage pair {65280, ',
    '[CLIoHidInterface] Service for usage pair {65280, ',
)
GIVE_UP = '[CLIoHidInterface] Could not set all properties after too many retries. Giving up.'
USAGE = r'\{65292, (?P<usage>\d+)\}'
CLEARED = r'\{65292, (?P<usage>1|7|26)\}'
DEVICE_MOTION = '[CLIoHidInterface] Property for usage pair {65292, 5}: {ReportInterval = '
FASTPATH_OPEN = '[FastPath] openFastPathPrivate() failed'
FASTPATH_COPY = '[FastPath] copyEventPrivate() Failed'
CACHE_COPY = '!locationd'
IOS27 = 'motion.kernel darwin=25'
# An iPadOS 27 boot: one whose kernel version line says Darwin 25, so an archive of several boots judges each its way.
IOS27_BOOT = 'motion.ios27-boot'
FASTPATH = 'drivers/SwitchTouch/SwitchHIDFastPath.cpp'
MOTION_ACCESS = 'drivers/SwitchTouch/SwitchHIDMotionAccess.cpp'
# IOKit's refusal, logged at default level (24B119 IOKit 0x18f591228-0x18f591298: os_log type 0, format
# "Connection: %@ denied match of service %@: TCC %@ right:%llu"); its arguments are not public, so the archive has them
# as <private>.
TCC_DENIED = (
    r'^Connection: (?P<connection>.+?) denied match of service (?P<service>.+?): '
    r'TCC (?P<tcc>kTCCServiceMotionSensors|<private>) right:(?P<right>\d+)$'
)
EDITS = 'translate/arm64e/pretranslate/edits_24B119.json'

MOTION = [
    Check(
        'motion.kernel',
        OWNER,
        'Darwin Kernel Version ',
        REPORT,
        process='kernel',
        regex=r'^Darwin Kernel Version (?P<darwin>\d+)\.',
        show=('darwin',),
        source=SOURCE,
        note="the boot's Darwin major: 25 is iPadOS 27, where locationd and the fast path's copies are judged",
    ),
    Check(
        IOS27_BOOT,
        OWNER,
        'Darwin Kernel Version 25.',
        REPORT,
        process='kernel',
        source=SOURCE,
        note='an iPadOS 27 boot began',
    ),
    Check(
        'motion.spu-added',
        OWNER,
        ADDED,
        process=CACHE_COPY,
        subsystem=SUBSYSTEM,
        regex=CLEARED,
        show=('usage',),
        source=SOURCE,
        note='a coprocessor service CoreMotion no longer asks for; boot 2: 26 x2',
    ),
    Check(
        'motion.spu-not-ready',
        OWNER,
        NOT_READY,
        process=CACHE_COPY,
        subsystem=SUBSYSTEM,
        regex=USAGE,
        show=('usage',),
        source=SOURCE,
        note='boot 2: usage 5 x25, 26 x41',
    ),
    *(
        Check(
            f'motion.give-up.{process}',
            OWNER,
            GIVE_UP,
            process=process,
            subsystem=SUBSYSTEM,
            source=SOURCE,
            note=f'boot 2: {count}',
        )
        for process, count in (('SpringBoard', 15), ('backboardd', 6), ('SwitchHWTest', 1))
    ),
    Check(
        'motion.locationd-spu-added',
        OWNER,
        ADDED,
        process='locationd',
        subsystem=SUBSYSTEM,
        regex=CLEARED,
        show=('usage',),
        applies=IOS27,
        needs='motion.kernel',
        boots=IOS27_BOOT,
        source=EDITS,
        note="a coprocessor service locationd's own table no longer asks for; boot 2, before 15032ab57: 7",
    ),
    Check(
        'motion.locationd-spu-not-ready',
        OWNER,
        NOT_READY,
        process='locationd',
        subsystem=SUBSYSTEM,
        regex=USAGE,
        show=('usage',),
        applies=IOS27,
        needs='motion.kernel',
        boots=IOS27_BOOT,
        source=EDITS,
        note="boot 2, before 15032ab57: 24; usage 5 here is every app's device motion",
    ),
    Check(
        'motion.give-up.locationd',
        OWNER,
        GIVE_UP,
        process='locationd',
        subsystem=SUBSYSTEM,
        applies=IOS27,
        needs='motion.kernel',
        boots=IOS27_BOOT,
        source=EDITS,
        note='boot 2, before 15032ab57: 2',
    ),
    Check(
        'motion.fastpath-null',
        OWNER,
        '[FastPath] Failed to open because of null service ref',
        subsystem=SUBSYSTEM,
        source=SOURCE,
        note='the 6-axis coprocessor service opened in process; boot 2: SpringBoard 2, SwitchHWTest 1',
    ),
    Check(
        'motion.device-motion-rejected',
        OWNER,
        (
            'Bad device motion report',
            'Unexpected device motion report type',
            'Device motion not initialized',
            '#Warning Device motion event ref invalid',
        ),
        subsystem=SUBSYSTEM,
        source='drivers/SwitchMotion/SwitchMotionFusion.h',
        note="CoreMotion refused SwitchMotion's device motion report (length, type or the initialized bit)",
    ),
    Check(
        'motion.device-motion-interval',
        OWNER,
        DEVICE_MOTION,
        REPORT,
        subsystem=SUBSYSTEM,
        regex=r'ReportInterval = (?P<us>\d+)',
        show=('us',),
        source='drivers/SwitchMotion/SwitchMotion.cpp',
        note="SwitchMotion's device motion service taking a client's interval",
    ),
    *(
        Check(
            f'motion.fastpath-open.{process}',
            OWNER,
            FASTPATH_OPEN,
            process=process,
            subsystem=SUBSYSTEM,
            applies=applies,
            needs='motion.kernel' if applies else '',
            boots=IOS27_BOOT if applies else '',
            source=FASTPATH,
            note='CoreMotion could not open the device motion fast path: the plug-in type, openForClient or the slots',
        )
        for process, applies in (('SpringBoard', ''), ('locationd', IOS27))
    ),
    Check(
        'motion.fastpath-open.backboardd',
        OWNER,
        FASTPATH_OPEN,
        REPORT,
        process='backboardd',
        subsystem=SUBSYSTEM,
        source=FASTPATH,
        note="expected: backboardd's 24B119 sandbox profile denies it IOHIDEventServiceFastPathUserClient, as on J181",
    ),
    *(
        Check(
            f'motion.fastpath-copy.{process}',
            OWNER,
            FASTPATH_COPY,
            process=process,
            subsystem=SUBSYSTEM,
            unless='motion.device-motion-interval',
            unless_within=1.0,
            key='process',
            applies=IOS27,
            needs='motion.kernel',
            boots=IOS27_BOOT,
            source=FASTPATH,
            note='a fast path copy that found no report, more than 1 s from its process setting a ReportInterval',
        )
        for process in ('SpringBoard', 'locationd')
    ),
    Check(
        'motion.fastpath-copy',
        OWNER,
        FASTPATH_COPY,
        REPORT,
        subsystem=SUBSYSTEM,
        show=('process',),
        source=FASTPATH,
        note="every process's failed copies; SpringBoard's and locationd's are judged on iPadOS 27",
    ),
    Check(
        'motion.tcc-denied',
        OWNER,
        ' denied match of service ',
        regex=TCC_DENIED,
        show=('connection', 'tcc', 'right'),
        debug=True,
        applies=IOS27,
        needs='motion.kernel',
        boots=IOS27_BOOT,
        source=MOTION_ACCESS,
        note="IOKit's event system refused a connection the Motion & Fitness gated services: not a platform "
        'binary, and TCC did not allow it',
    ),
    Check(
        'motion.gated-not-ready',
        OWNER,
        GATED_NOT_READY,
        regex=r'\{65280, (?P<usage>3|9)\}',
        show=('process', 'usage'),
        applies=IOS27,
        needs='motion.kernel',
        boots=IOS27_BOOT,
        source=MOTION_ACCESS,
        note='a CoreMotion client asked for the accelerometer or gyroscope and found no service, as a refusal of the '
        "gated service leaves it (the device motion service's are motion.spu-not-ready's)",
    ),
    Check(
        'motion.spu-added-ungated',
        OWNER,
        ADDED,
        REPORT,
        subsystem=SUBSYSTEM,
        regex=r'\{65292, (?P<usage>4|8|24)\}',
        show=('usage',),
        source=SOURCE,
        note='boot 2: locationd 4 x2, 8 x2, 24 x2',
    ),
]
