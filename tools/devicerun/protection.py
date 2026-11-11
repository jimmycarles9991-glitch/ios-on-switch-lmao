"""iOS 27's Data volume read as a content-protected one (kernel/switch/vfs/protection.c).

Boot 15 (11-08) is the baseline: libsqlite stripped SQLITE_OPEN_FILEPROTECTION_* 4,174 times, locationd crashed twice
right after its CLDataProtectionManager logged 'content protection formatting, 0', and spotlightknowledged's updater
died on its DocumentProcessing probe reading as NSURLFileProtectionNone.
"""

from tools.devicerun.rules import REPORT, Check

OWNER = 'r122-crashsys'
KERNEL = 'kernel'
SOURCE = 'kernel/switch/vfs/protection.c'
ON = 'protection.kernel state=on'

PROTECTION = [
    Check(
        'protection.kernel',
        OWNER,
        'SwitchProtection: ',
        REPORT,
        process=KERNEL,
        regex=r'SwitchProtection: (?P<state>on|off)',
        show=('state',),
        source=SOURCE,
        note='iOS 27 kernels only',
    ),
    Check('protection.off', OWNER, 'SwitchProtection: off', process=KERNEL, source=SOURCE),
    Check(
        'protection.sqlite',
        OWNER,
        'volume does not support data protection',
        applies=ON,
        source='libsqlite3 (dyld cache)',
        note='was 4,174 in boot 15',
    ),
    Check(
        'protection.locationd',
        OWNER,
        'content protection formatting, 0',
        process='locationd',
        applies=ON,
        source='locationd',
    ),
    Check(
        'protection.spotlight',
        OWNER,
        'has unexpected protection class',
        process='spotlightknowledged.updater',
        applies=ON,
        source='SpotlightKnowledgeDaemon (dyld cache)',
    ),
    Check(
        'protection.crashes',
        OWNER,
        ('SwitchCrash: locationd[', 'SwitchCrash: spotlightknowledged'),
        process=KERNEL,
        regex=r'SwitchCrash: (?P<who>[\w.]+)\[\d+\] exit reason',
        show=('who',),
        source='kernel/switch/diag/crash.c',
        note='this boot only; boot 15: locationd x2, spotlightknowledged.updater x1',
    ),
]
