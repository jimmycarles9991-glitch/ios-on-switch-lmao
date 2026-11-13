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
    Check(
        'dispatch.early-sync-wait',
        OWNER,
        'returned at once on a knote already woken for',
        process=KERNEL,
        regex=r'SwitchWL: (?P<who>\S+\[\d+\]) thread .* woken for (?P<waiter>this|another) thread',
        show=('who', 'waiter'),
        source='kernel/switch/diag/workloop.c',
        note="a sync waiter given ownership by an earlier wait's wake; before a locationd SwitchCrash, the kernel side",
    ),
    Check(
        'dispatch.cpu-hog',
        OWNER,
        'SwitchCPU: user pc ',
        process=KERNEL,
        regex=r'user pc (?P<pc>(?:dsc |main\+)?0x[0-9a-f]+) lr (?P<lr>(?:dsc |main\+)?0x[0-9a-f]+)',
        show=('pc', 'lr'),
        source='kernel/switch/diag/crash.c',
        note=(
            'a thread over its CPU limit, where its loop runs; slice KU (11-10): locationd thread 0x624, 50% for 3 min, '
            'before this line existed'
        ),
    ),
]
