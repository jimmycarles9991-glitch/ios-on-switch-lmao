"""iPadOS 27's motion capabilities: switch_ui_scale_sites_24B119.h's CoreMotion site.

CoreMotion takes J181's word without its motion coprocessor's bits, so it no longer opens the coprocessor's page-0xff0c
services and fuses device motion on the CPU from SwitchMotion's accelerometer and gyroscope.

Boot 2 (22-23 Nov, iOS 27) is the baseline: 90 "not ready" lines for usages 1, 5 and 26 and 24 give-ups (locationd 2,
SpringBoard 15, backboardd 6, SwitchHWTest 1); locationd also added usages 4, 7, 8 and 24 without setting a property.
Usages 4, 8 and 24 are reported, not judged: their creation gates were not read. The checks judge the processes that
run CoreMotion's table from the cache; locationd has its own copy of it, outside the cache.
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
GIVE_UP = '[CLIoHidInterface] Could not set all properties after too many retries. Giving up.'
USAGE = r'\{65292, (?P<usage>\d+)\}'
CLEARED = r'\{65292, (?P<usage>1|5|7|26)\}'
CACHE_COPY = '!locationd'

MOTION = [
    Check(
        'motion.spu-added',
        OWNER,
        ADDED,
        process=CACHE_COPY,
        subsystem=SUBSYSTEM,
        regex=CLEARED,
        show=('usage',),
        source=SOURCE,
        note='a coprocessor service CoreMotion no longer asks for; boot 2: 5 x2, 26 x2',
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
        'motion.fastpath-null',
        OWNER,
        '[FastPath] Failed to open because of null service ref',
        subsystem=SUBSYSTEM,
        source=SOURCE,
        note='the 6-axis coprocessor service opened in process; boot 2: SpringBoard 2, SwitchHWTest 1',
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
    Check(
        'motion.locationd-spu',
        OWNER,
        (ADDED, *NOT_READY, GIVE_UP),
        REPORT,
        process='locationd',
        subsystem=SUBSYSTEM,
        regex=r'(?:\{65292, (?P<usage>1|5|7|26)\}|Giving up\.)',
        show=('usage',),
        source='translate/arm64e/pretranslate/edits_24B119.json',
        note='report until 24B119 os.trustcache is regenerated with edits_24B119.json; boot 2: 24 not-ready, 2 give-ups',
    ),
]
