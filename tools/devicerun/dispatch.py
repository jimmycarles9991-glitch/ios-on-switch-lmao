"""libdispatch's serial queues on iOS 27: a dispatch_sync waiter's context reached after its thread has left it.

Boot 18 (11-09) is the baseline: locationd started three times (pids 73, 196, 212). The first two died seconds in
when their cohort queue's drainer retained a voucher read from a reused stack, CFSTR("com.apple.locationd"), a write
into CoreFoundation's __AUTH_CONST. reports.py's crash.stale-sync-waiter row reads the same crashes from the .ips files.
"""

from tools.devicerun.rules import Check

OWNER = 'r132-locationd'
KERNEL = 'kernel'

DISPATCH = [
    Check(
        'dispatch.locationd-starts',
        OWNER,
        '"msg":"locationd main - setup"',
        'count <= 1',
        process='locationd',
        source='locationd',
        note='any restart fails it; boot 18: 3 (two crashed on their cohort queue); crash.stale-sync-waiter names why',
    ),
    Check(
        'dispatch.shared-cache-fault',
        OWNER,
        '; far dsc ',
        process=KERNEL,
        regex=r'user pc dsc (?P<pc>0x[0-9a-f]+) lr dsc (?P<lr>0x[0-9a-f]+)',
        show=('pc', 'lr'),
        source='kernel/switch/diag/crash.c',
        note='a crash whose fault address is in the dyld cache; boot 18: locationd x2, lr dsc 0x1af3cfbbc (24B119)',
    ),
]
